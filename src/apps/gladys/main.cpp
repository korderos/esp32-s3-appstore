// Énergie Gladys — Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B 172x640, QSPI), paysage
// Production solaire, consommation, réseau et températures lues dans Gladys Assistant.
//
// Les données passent par le service MCP de Gladys (POST /api/v1/service/mcp/proxy,
// outil "device_get_state") : il suffit d'une clé API Gladys avec le droit "mcp".
// Adresse, clé et choix des capteurs se règlent sur http://energie.local (jamais dans
// le firmware : l'appstore et le dépôt sont publics).
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>

#include "user_config.h"
#include "lvgl_port.h"
#include "i2c_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "ui.h"
#include "../common/wifi_shared.h"
#include "../common/screenshot.h"

#define AP_NAME        "Energie-Setup"
#define MDNS_NAME      "energie"
#define POLL_MS        15000UL   // puissances
#define SAMPLE_MS      60000UL   // un point de courbe par minute
#define TZ_PARIS       "CET-1CEST,M3.5.0,M10.5.0/3"
#define DEFAULT_URL    "http://umbrel.local:8090"

static Preferences prefs;
static WebServer web(80);
static DNSServer dns;
static bool setupMode = false, forcePortal = false;
static String savedSsid, savedPass;

// ------------------------------------------------------------------ réglages
// Chaque mesure = "Nom de l'appareil|Nom de la fonction", tels qu'affichés dans Gladys.
struct Slot { const char *key, *label, *type, *def; };
static const Slot SLOTS[] = {
  {"m_prod",  "Production solaire (puissance)",  "energy-sensor",      ""},
  {"m_pidx",  "Production solaire (index kWh)",  "energy-sensor",      ""},
  {"m_cons",  "Consommation (puissance)",        "energy-sensor",      "Enphase gateway|Consumption"},
  {"m_cday",  "Consommation du jour (kWh)",      "energy-sensor",      "Enphase gateway|Consumption today"},
  {"m_grid",  "Réseau (puissance, + = achat)",   "energy-sensor",      ""},
  {"m_imp",   "Réseau : index achat (kWh)",      "energy-sensor",      ""},
  {"m_exp",   "Réseau : index injection (kWh)",  "energy-sensor",      ""},
  {"m_text",  "Température extérieure",          "temperature-sensor", "Temp Ext|Température"},
  {"m_tint",  "Température intérieure",          "temperature-sensor", "Temp Int|Température"},
};
enum { S_PROD, S_PIDX, S_CONS, S_CDAY, S_GRID, S_IMP, S_EXP, S_TEXT, S_TINT, NSLOTS };
static String slotVal[NSLOTS];
static String gladysUrl, apiKey;

// ------------------------------------------------------------------ utilitaires
struct UiLock {
  UiLock() { lvgl_lock(-1); }
  ~UiLock() { lvgl_unlock(); }
};

static void status(const char *txt, bool ok) {
  UiLock l;
  ui_set_status(txt, ok);
}

static String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

// Les polices de l'écran n'ont pas d'accents : translittération simple (UTF-8 -> ASCII).
static String toAscii(const String &in) {
  static const char *LAT1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  String o;
  const uint8_t *p = (const uint8_t *)in.c_str();
  while (*p) {
    if (*p < 0x80) { o += (char)*p++; continue; }
    if ((*p & 0xE0) == 0xC0 && p[1]) {
      uint32_t cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
      if (cp >= 0xC0 && cp <= 0xFF) o += LAT1[cp - 0xC0];
      continue;
    }
    p++;
  }
  return o;
}

// ------------------------------------------------------------------ accès à Gladys
static String baseUrl;  // URL avec l'adresse IP si le nom se termine par .local

// L'ESP32 ne résout pas tout seul les noms .local : on interroge mDNS.
static bool resolveBase() {
  String u = gladysUrl;
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  int s = u.indexOf("://");
  int hostStart = s < 0 ? 0 : s + 3;
  int hostEnd = u.indexOf(':', hostStart);
  int slash = u.indexOf('/', hostStart);
  if (hostEnd < 0 || (slash >= 0 && slash < hostEnd)) hostEnd = slash < 0 ? u.length() : slash;
  String host = u.substring(hostStart, hostEnd);
  if (host.endsWith(".local")) {
    String shortName = host.substring(0, host.length() - 6);
    IPAddress ip = MDNS.queryHost(shortName, 3000);
    // repli : la plupart des box résolvent aussi le nom court via leur DNS (ex. umbrel -> umbrel.lan)
    if (ip == IPAddress(0, 0, 0, 0) && !WiFi.hostByName(shortName.c_str(), ip)) ip = IPAddress(0, 0, 0, 0);
    if (ip == IPAddress(0, 0, 0, 0)) {
      Serial.printf("[gladys] %s introuvable (mDNS et DNS)\n", host.c_str());
      return false;
    }
    u = u.substring(0, hostStart) + ip.toString() + u.substring(hostEnd);
  }
  baseUrl = u;
  Serial.printf("[gladys] %s\n", baseUrl.c_str());
  return true;
}

// Appelle l'outil MCP "device_get_state" ; renvoie le tableau texte de Gladys.
// Code d'erreur : 0 = ok, 1 = réseau, 2 = clé refusée, 3 = réponse illisible
static int mcpDeviceState(const char *type, String &table) {
  if (!baseUrl.length() && !resolveBase()) return 1;
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(baseUrl + "/api/v1/service/mcp/proxy")) return 1;
  http.addHeader("Authorization", apiKey);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json, text/event-stream");
  String body = String("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"device_get_state\",\"arguments\":{\"device_type\":\"") + type + "\"}}}";
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  if (code <= 0) { baseUrl = ""; return 1; }  // l'adresse a peut-être changé : nouvelle résolution
  if (code == 401 || code == 403) return 2;
  if (code != 200) { Serial.printf("[gladys] HTTP %d\n", code); return 1; }
  // réponse au format SSE : "event: message\ndata: {...}"
  int d = resp.indexOf("data:");
  String json = d >= 0 ? resp.substring(d + 5) : resp;
  int end = json.indexOf('\n');
  if (end > 0) json = json.substring(0, end);
  JsonDocument doc;
  if (deserializeJson(doc, json)) return 3;
  if (doc["result"]["isError"] | false) return 3;
  table = doc["result"]["content"][0]["text"] | "";
  return table.length() ? 0 : 3;
}

struct Reading { bool ok; float value; String unit, age; };

// Cherche "Appareil|Fonction" dans le tableau : lignes "piece,appareil,fonction,categorie,valeur,unite,age".
static Reading lookup(const String &table, const String &slot) {
  Reading r = {false, 0, "", ""};
  int bar = slot.indexOf('|');
  if (bar < 0) return r;
  String dev = slot.substring(0, bar), feat = slot.substring(bar + 1);
  dev.trim();
  feat.trim();
  int pos = 0;
  while (pos < (int)table.length()) {
    int nl = table.indexOf('\n', pos);
    if (nl < 0) nl = table.length();
    String line = table.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    // découpe : 1er champ = pièce, 4 derniers = catégorie, valeur, unité, âge, le reste = appareil,fonction
    int c[64], n = 0;
    for (int i = 0; i < (int)line.length() && n < 64; i++) if (line[i] == ',') c[n++] = i;
    if (n < 6) continue;
    String d = line.substring(c[0] + 1, c[1]);
    String f = line.substring(c[1] + 1, c[n - 4]);
    if (d != dev || f != feat) continue;
    String val = line.substring(c[n - 3] + 1, c[n - 2]);
    if (val == "null" || !val.length()) return r;
    r.ok = true;
    r.value = val.toFloat();
    r.unit = line.substring(c[n - 2] + 1, c[n - 1]);
    r.age = line.substring(c[n - 1] + 1);
    return r;
  }
  return r;
}

static float toKw(const Reading &r) { return r.unit == "watt" ? r.value / 1000.0f : r.value; }
static float toKwh(const Reading &r) { return r.unit == "watt-hour" ? r.value / 1000.0f : r.value; }

// "30min" -> "30 min", "17h" -> "17 h", "2d" -> "2 j"
static void fmtAge(const String &a, char *out, size_t n) {
  int v = a.toInt();
  if (a.endsWith("min")) { if (v == 0) snprintf(out, n, "< 1 min"); else snprintf(out, n, "%d min", v); }
  else if (a.endsWith("h")) snprintf(out, n, "%d h", v);
  else if (a.endsWith("d")) snprintf(out, n, "%d j", v);
  else snprintf(out, n, "%s", a.c_str());
}

// ------------------------------------------------------------------ énergies du jour
// Production, achat et injection du jour = index actuel - index relevé en début de journée.
static int baseDay = 0;
static float baseP = 0, baseI = 0, baseE = 0;
static char baseSince[6] = "";

static int today(struct tm &tm) {
  if (!getLocalTime(&tm, 10)) return 0;
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static bool dailyCounters(const Reading &p, const Reading &i, const Reading &e, EnergyData &d) {
  struct tm tm;
  int day = today(tm);
  if (!day || !p.ok || !i.ok || !e.ok) return false;
  if (day != baseDay) {
    baseDay = day;
    baseP = toKwh(p);
    baseI = toKwh(i);
    baseE = toKwh(e);
    // relevé juste après minuit = journée complète, sinon on affiche "depuis HH:MM"
    if (tm.tm_hour == 0 && tm.tm_min < 5) baseSince[0] = 0;
    else snprintf(baseSince, sizeof(baseSince), "%02d:%02d", tm.tm_hour, tm.tm_min);
    prefs.putInt("bday", baseDay);
    prefs.putFloat("bp", baseP);
    prefs.putFloat("bi", baseI);
    prefs.putFloat("be", baseE);
    prefs.putString("bsince", baseSince);
  }
  d.hasDaily = true;
  d.prodTodayKwh = fmaxf(toKwh(p) - baseP, 0);
  d.importTodayKwh = fmaxf(toKwh(i) - baseI, 0);
  d.exportTodayKwh = fmaxf(toKwh(e) - baseE, 0);
  snprintf(d.dailySince, sizeof(d.dailySince), "%s", baseSince);
  return true;
}

// ------------------------------------------------------------------ lecture complète
static String lastEnergy, lastTemp;  // derniers tableaux (affichés sur la page de réglages)
static EnergyData data;

static int refreshData(bool withTemps) {
  String t;
  int err = mcpDeviceState("energy-sensor", t);
  if (err) return err;
  lastEnergy = t;
  if (withTemps || !lastTemp.length()) {
    String tt;
    if (mcpDeviceState("temperature-sensor", tt) == 0) lastTemp = tt;
  }
  EnergyData d = {};
  Reading r;
  r = lookup(t, slotVal[S_PROD]); d.hasProd = r.ok; d.prodKw = toKw(r);
  r = lookup(t, slotVal[S_CONS]); d.hasCons = r.ok; d.consKw = toKw(r);
  r = lookup(t, slotVal[S_GRID]); d.hasGrid = r.ok; d.gridKw = toKw(r);
  if (!d.hasGrid && d.hasProd && d.hasCons) { d.hasGrid = true; d.gridKw = d.consKw - d.prodKw; }
  r = lookup(t, slotVal[S_CDAY]); d.hasConsToday = r.ok; d.consTodayKwh = toKwh(r);
  dailyCounters(lookup(t, slotVal[S_PIDX]), lookup(t, slotVal[S_IMP]), lookup(t, slotVal[S_EXP]), d);
  r = lookup(lastTemp, slotVal[S_TEXT]); d.hasExt = r.ok; d.tExt = r.value; fmtAge(r.age, d.ageExt, sizeof(d.ageExt));
  r = lookup(lastTemp, slotVal[S_TINT]); d.hasInt = r.ok; d.tInt = r.value; fmtAge(r.age, d.ageInt, sizeof(d.ageInt));
  data = d;
  UiLock l;
  ui_set_data(d);
  ui_set_status("OK", true);
  return 0;
}

// ------------------------------------------------------------------ pages web
static String scanOptions;

static void handleRoot() {
  String page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Énergie Gladys</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b0e14;color:#eee;max-width:560px;margin:auto;padding:24px}"
    "input,button{width:100%;padding:11px;margin:4px 0 14px;border-radius:8px;border:1px solid #333;"
    "background:#151a23;color:#eee;font-size:15px;box-sizing:border-box;font-family:inherit}"
    "button{background:#3b82f6;border:0;font-weight:600;font-size:16px}label{font-size:14px;color:#bbb}"
    "small{color:#7d8696;display:block;margin:-8px 0 14px}h3{margin-top:26px}"
    "pre{background:#151a23;padding:10px;border-radius:8px;font-size:12px;overflow-x:auto;white-space:pre}"
    "</style></head><body><h2>Énergie Gladys &ndash; réglages</h2><form method='POST' action='/save'>";
  if (setupMode) {
    page += "<h3>Wi-Fi</h3><label>Réseau</label><input name='s' list='nets' required autocomplete='off' value='" +
            htmlEscape(savedSsid) + "'><datalist id='nets'>" + scanOptions + "</datalist>"
            "<label>Mot de passe</label><input name='p' type='password'>";
  }
  page += "<h3>Gladys</h3><label>Adresse de Gladys</label><input name='url' value='" + htmlEscape(gladysUrl) + "'>"
          "<label>Clé API (droit « mcp »)</label><input name='key' type='password' autocomplete='off' placeholder='" +
          String(apiKey.length() ? "enregistrée — laisser vide pour la garder" : "à saisir") + "'>"
          "<small>Gladys &rarr; Paramètres &rarr; Clés API. La clé reste dans le module.</small>"
          "<h3>Capteurs</h3><small>Format : <b>Appareil|Fonction</b>, comme dans le tableau en bas de page. Vide = non affiché.</small>";
  for (int i = 0; i < NSLOTS; i++)
    page += "<label>" + String(SLOTS[i].label) + "</label><input name='" + SLOTS[i].key + "' value='" + htmlEscape(slotVal[i]) + "'>";
  page += "<button type='submit'>Enregistrer et redémarrer</button></form>";
  if (!setupMode) {
    page += "<form method='POST' action='/wifi' onsubmit=\"return confirm('Oublier le Wi-Fi ?')\">"
            "<button style='background:#ea3943'>Changer de réseau Wi-Fi</button></form>";
  }
  if (lastEnergy.length() || lastTemp.length())
    page += "<h3>Valeurs lues dans Gladys</h3><pre>" + htmlEscape(lastEnergy) + "\n\n" + htmlEscape(lastTemp) + "</pre>";
  page += "</body></html>";
  web.send(200, "text/html; charset=utf-8", page);
}

static void sendDone(const char *msg) {
  String p = "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<body style='font-family:sans-serif;background:#0b0e14;color:#eee;padding:24px'><h2>Enregistré</h2><p>";
  p += msg;
  p += "</p>";
  web.send(200, "text/html; charset=utf-8", p);
}

static void handleSave() {
  if (web.hasArg("s") && web.arg("s").length()) wifi_shared_save(web.arg("s"), web.arg("p"));
  if (web.hasArg("url")) prefs.putString("url", web.arg("url"));
  if (web.hasArg("key") && web.arg("key").length()) prefs.putString("key", web.arg("key"));
  for (int i = 0; i < NSLOTS; i++)
    if (web.hasArg(SLOTS[i].key)) prefs.putString(SLOTS[i].key, web.arg(SLOTS[i].key));
  sendDone("Le module redémarre...");
  delay(1500);
  ESP.restart();
}

static void handleWifiReset() {
  sendDone("Le module redémarre en mode configuration : connectez-vous au Wi-Fi « " AP_NAME " ».");
  g_reqWifiReset = true;
}

static void setupWebRoutes() {
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/wifi", HTTP_POST, handleWifiReset);
}

static void startSetupPortal() {
  setupMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  scanOptions = "";
  for (int i = 0; i < n; i++) scanOptions += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  if (savedSsid.length() && !forcePortal) WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  setupWebRoutes();
  web.onNotFound([] {
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
  UiLock l;
  ui_show_message("Configuration",
                  "1. Connectez-vous au reseau Wi-Fi \"" AP_NAME "\"\n"
                  "2. Ouvrez http://192.168.4.1 (la page s'ouvre souvent seule)\n"
                  "3. Choisissez votre Wi-Fi, l'adresse et la cle API de Gladys");
}

static bool connectWifi() {
  if (!savedSsid.length()) return false;
  {
    UiLock l;
    String m = "Reseau : " + toAscii(savedSsid);
    ui_show_message("Connexion Wi-Fi...", m.c_str());
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) improv_delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ------------------------------------------------------------------ orientation (IMU QMI8658)
#define IMU_AXIS       1
#define IMU_AXIS_SIGN  -1
static bool imuOk = false;

static void imu_init() {
  uint8_t id = 0;
  if (i2c_read_buff(imu_dev_handle, 0x00, &id, 1) != ESP_OK || id != 0x05) return;
  uint8_t v;
  v = 0x40; i2c_write_buff(imu_dev_handle, 0x02, &v, 1);
  v = 0x15; i2c_write_buff(imu_dev_handle, 0x03, &v, 1);
  v = 0x01; i2c_write_buff(imu_dev_handle, 0x08, &v, 1);
  imuOk = true;
}

static void imu_poll() {
  static uint32_t last = 0;
  static int votes = 0;
  if (!imuOk || millis() - last < 200) return;
  last = millis();
  uint8_t b[6];
  if (i2c_read_buff(imu_dev_handle, 0x35, b, 6) != ESP_OK) return;
  int16_t ax = (int16_t)(b[1] << 8 | b[0]);
  int16_t ay = (int16_t)(b[3] << 8 | b[2]);
  int16_t az = (int16_t)(b[5] << 8 | b[4]);
  int a = (IMU_AXIS == 0 ? ax : ay) * IMU_AXIS_SIGN;
  int other = (IMU_AXIS == 0 ? ay : ax);
  if (abs(a) < 4900 || abs(a) < 2 * abs(other) || abs(a) < abs(az)) { votes = 0; return; }
  bool wantFlip = a < 0;
  if (wantFlip == lvgl_get_flip()) { votes = 0; return; }
  if (++votes < 4) return;
  votes = 0;
  UiLock l;
  lvgl_set_flip(wantFlip);
}

// ------------------------------------------------------------------ batterie
#define BAT_ADC_PIN 4
static i2c_master_dev_handle_t tca_dev = NULL;

static void power_init() {
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK) return;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = 0x20;
  cfg.scl_speed_hz = 300000;
  if (i2c_master_bus_add_device(bus, &cfg, &tca_dev) != ESP_OK) return;
  uint8_t out = 0xFF, dir = 0xFF & ~(1 << 6);
  i2c_write_buff(tca_dev, 0x01, &out, 1);
  i2c_write_buff(tca_dev, 0x03, &dir, 1);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}

static int battery_percent(float v) {
  static const float tbl[][2] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75}, {3.95f, 70},
    {3.91f, 65},  {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45}, {3.80f, 40}, {3.79f, 35},
    {3.77f, 30},  {3.75f, 25}, {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},  {3.30f, 0}};
  const int n = sizeof(tbl) / sizeof(tbl[0]);
  if (v >= tbl[0][0]) return 100;
  if (v <= tbl[n - 1][0]) return 0;
  for (int i = 1; i < n; i++) {
    if (v >= tbl[i][0]) {
      float t = (v - tbl[i][0]) / (tbl[i - 1][0] - tbl[i][0]);
      return (int)lroundf(tbl[i][1] + t * (tbl[i - 1][1] - tbl[i][1]));
    }
  }
  return 0;
}

static void battery_poll() {
  static uint32_t last = 0;
  static float filt = -1;
  if (last && millis() - last < 2000) return;
  last = millis();
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  float v = mv / 16.0f * 3.0f / 1000.0f;
  filt = filt < 0 ? v : filt * 0.8f + v * 0.2f;
  UiLock l;
  ui_set_battery(battery_percent(filt), Serial.isPlugged());
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  improv_begin("Énergie Gladys", "1.0.0", "http://" MDNS_NAME ".local");
  improv_set_line_hook(screenshot_handle_line);
  improv_delay(1500);
  Serial.println("[boot] Energie Gladys");
  i2c_master_Init();
  power_init();
  lvgl_port_init();
  screenshot_begin(lvgl_lock, lvgl_unlock);
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  prefs.begin("gladys", false);
  wifi_shared_load(savedSsid, savedPass);
  forcePortal = prefs.getBool("portal", false);
  if (forcePortal) prefs.remove("portal");
  gladysUrl = prefs.getString("url", DEFAULT_URL);
  apiKey = prefs.isKey("key") ? prefs.getString("key", "") : String();
  for (int i = 0; i < NSLOTS; i++) slotVal[i] = prefs.isKey(SLOTS[i].key) ? prefs.getString(SLOTS[i].key, "") : String(SLOTS[i].def);
  baseDay = prefs.getInt("bday", 0);
  baseP = prefs.getFloat("bp", 0);
  baseI = prefs.getFloat("bi", 0);
  baseE = prefs.getFloat("be", 0);
  snprintf(baseSince, sizeof(baseSince), "%s", prefs.getString("bsince", "").c_str());
  imu_init();
  {
    UiLock l;
    ui_init();
  }

  if (forcePortal || !connectWifi()) {
    startSetupPortal();
    return;
  }
  configTzTime(TZ_PARIS, "pool.ntp.org", "time.google.com");
  String ip = WiFi.localIP().toString();
  Serial.printf("[wifi] connecte, IP %s\n", ip.c_str());
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  setupWebRoutes();
  web.begin();

  String info = "Adresse de Gladys, cle API et capteurs :\n"
                "   http://" MDNS_NAME ".local   ou   http://" + ip + "\n\n"
                "Gladys : " + gladysUrl + "\n"
                "Mise a jour toutes les 15 s, courbe sur 3 h.";
  UiLock l;
  ui_set_info(info.c_str());
  ui_show_main();
  if (!apiKey.length()) {
    String m = "Saisissez l'adresse et la cle API de Gladys sur\nhttp://" MDNS_NAME ".local  ou  http://" + ip;
    ui_show_message("Cle API Gladys", m.c_str());
  }
}

void loop() {
  if (g_reqWifiReset) {
    wifi_shared_forget();
    prefs.putBool("portal", true);
    delay(300);
    ESP.restart();
  }
  improv_poll();
  imu_poll();
  battery_poll();

  if (setupMode) {
    dns.processNextRequest();
    web.handleClient();
    if (WiFi.status() == WL_CONNECTED) ESP.restart();
    delay(5);
    return;
  }
  web.handleClient();
  if (!apiKey.length()) { delay(20); return; }

  if (WiFi.status() != WL_CONNECTED) {
    status("Wi-Fi hors ligne", false);
    delay(500);
    return;
  }

  static uint32_t lastPoll = 0, lastTemp = 0, lastSample = 0;
  uint32_t now = millis();
  if (g_reqRefresh || !lastPoll || now - lastPoll >= POLL_MS) {
    bool temps = g_reqRefresh || !lastTemp || now - lastTemp >= SAMPLE_MS;
    g_reqRefresh = false;
    int err = refreshData(temps);
    lastPoll = now;
    if (temps && !err) lastTemp = now;
    if (err) {
      const char *msg = err == 2 ? "Cle API refusee" : err == 3 ? "Reponse Gladys illisible" : "Gladys injoignable";
      Serial.printf("[gladys] erreur %d\n", err);
      status(msg, false);
      if (err == 2) {  // inutile d'insister : on attend une nouvelle clé
        UiLock l;
        ui_show_message("Cle API refusee", "Gladys refuse la cle (elle doit avoir le droit \"mcp\").\n"
                                           "Corrigez-la sur http://" MDNS_NAME ".local");
      }
    } else {
      { UiLock l; ui_show_main(); }
      if (!lastSample || now - lastSample >= SAMPLE_MS) {
        lastSample = now;
        UiLock l;
        ui_add_sample(data.hasProd ? data.prodKw : 0, data.hasCons ? data.consKw : 0);
      }
    }
  }
  delay(20);
}
