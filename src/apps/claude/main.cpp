// Suivi Claude — Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B 172x640, QSPI), paysage
// Dépenses et consommation de l'organisation Claude, lues avec une clé Admin API
// (Usage and Cost Analytics API : /v1/organizations/analytics/cost et /usage, en bêta).
//
// La clé admin donne tous les droits sur l'organisation : elle se saisit sur http://claude.local,
// reste dans la NVS du module, n'est jamais réaffichée, et n'est envoyée qu'à api.anthropic.com
// après vérification de son certificat (racines Google Trust Services, certs.h).
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
#include "certs.h"
#include "../common/wifi_shared.h"
#include "../common/screenshot.h"

#define AP_NAME       "Claude-Setup"
#define MDNS_NAME     "claude"
#define API_HOST      "https://api.anthropic.com"
#define API_VERSION   "2026-10-21"
#define API_BETA      "analytics-usage-cost-2026-08-24"
#define POLL_MS       (15UL * 60 * 1000)   // les données arrivent avec 4 à 24 h de retard
#define RETRY_MS      (60UL * 1000)
#define TZ_PARIS      "CET-1CEST,M3.5.0,M10.5.0/3"

static Preferences prefs;
static WiFiClientSecure tls;
static WebServer web(80);
static DNSServer dns;
static bool setupMode = false, forcePortal = false;
static String savedSsid, savedPass, adminKey;
static float budgetUsd = 0;

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

// jours depuis le 01/01/1970 (algorithme de H. Hinnant), pour manipuler les dates UTC
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// "2026-11-22T14:02:11Z" -> secondes UTC
static time_t parseIso(const char *s) {
  if (!s || strlen(s) < 19) return 0;
  int y = atoi(s), mo = atoi(s + 5), d = atoi(s + 8), h = atoi(s + 11), mi = atoi(s + 14), se = atoi(s + 17);
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + se);
}

static String isoDay(time_t t) {  // minuit UTC du jour de t
  struct tm g;
  gmtime_r(&t, &g);
  char b[24];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT00:00:00Z", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
  return b;
}

// "claude-opus-5-5" -> "Opus 5.5", "claude-sonnet-4-5-20250929" -> "Sonnet 4.5", "claude-3-5-haiku-20241022" -> "Haiku 3.5"
static void prettyModel(const char *id, char *out, size_t n) {
  if (!id || !*id) { snprintf(out, n, "Autres frais"); return; }
  String s = id;
  if (s.startsWith("claude-")) s = s.substring(7);
  String family, ver;
  int start = 0;
  while (start <= (int)s.length()) {
    int dash = s.indexOf('-', start);
    if (dash < 0) dash = s.length();
    String tok = s.substring(start, dash);
    start = dash + 1;
    bool digits = tok.length() > 0;
    for (char c : tok) digits &= isdigit((unsigned char)c) != 0;
    if (digits && tok.length() >= 6) continue;  // date de version
    if (digits) ver += (ver.length() ? "." : "") + tok;
    else if (!family.length() && tok.length()) { family = tok; family.setCharAt(0, toupper(family[0])); }
  }
  if (!family.length()) snprintf(out, n, "%s", id);
  else snprintf(out, n, "%s%s%s", family.c_str(), ver.length() ? " " : "", ver.c_str());
}

// ------------------------------------------------------------------ Admin API
// Renvoie le code HTTP (négatif = erreur réseau / TLS).
static int apiGet(const String &pathAndQuery, String &out) {
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, String(API_HOST) + pathAndQuery)) return -1;
  http.addHeader("x-api-key", adminKey);
  http.addHeader("anthropic-version", API_VERSION);
  http.addHeader("anthropic-beta", API_BETA);
  int code = http.GET();
  if (code > 0) out = http.getString();
  if (code != 200) Serial.printf("[api] HTTP %d %s\n", code, code > 0 ? out.substring(0, 160).c_str() : "");
  http.end();
  return code;
}

static int apiJson(const String &pathAndQuery, JsonDocument &doc, JsonDocument &filter) {
  String body;
  int code = apiGet(pathAndQuery, body);
  if (code != 200) return code;
  // diagnostic sur le port USB uniquement (la réponse ne contient pas la clé)
  Serial.printf("[api] %s -> %u octets : %s\n", pathAndQuery.substring(0, pathAndQuery.indexOf('?')).c_str(),
                (unsigned)body.length(), body.substring(0, 400).c_str());
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return -2;
  return 200;
}

static float cents(JsonVariantConst v) {  // montants : chaînes décimales en cents
  const char *s = v | "0";
  return atof(s) / 100.0f;
}

static ClaudeData data;

// Lit les trois rapports ; renvoie 200 ou le premier code d'erreur.
static int refreshData() {
  time_t now = time(NULL);
  struct tm g;
  gmtime_r(&now, &g);
  char monthStart[24];
  snprintf(monthStart, sizeof(monthStart), "%04d-%02d-01T00:00:00Z", g.tm_year + 1900, g.tm_mon + 1);
  ClaudeData d = {};
  d.budgetUsd = budgetUsd;
  time_t refreshed = 0;

  // 1. coût du mois par modèle
  {
    JsonDocument filter;
    filter["data"][0]["group"]["model"] = true;
    filter["data"][0]["cost"]["amount"] = true;
    filter["data_refreshed_at"] = true;
    JsonDocument doc;
    int code = apiJson(String("/v1/organizations/analytics/cost?starting_at=") + monthStart +
                       "&group_by%5B%5D=model&limit=1000", doc, filter);
    if (code != 200) return code;
    refreshed = parseIso(doc["data_refreshed_at"] | "");
    // regroupe par nom affiché (les versions datées d'un même modèle se cumulent)
    struct Acc { char name[24]; float usd; } acc[16];
    int na = 0;
    for (JsonObjectConst row : doc["data"].as<JsonArrayConst>()) {
      char name[24];
      prettyModel(row["group"]["model"] | "", name, sizeof(name));
      float v = cents(row["cost"]["amount"]);
      d.monthUsd += v;
      int k = 0;
      while (k < na && strcmp(acc[k].name, name)) k++;
      if (k == na && na < 16) { snprintf(acc[na].name, sizeof(acc[na].name), "%s", name); acc[na++].usd = 0; }
      if (k < na) acc[k].usd += v;
    }
    for (int i = 0; i < na; i++)  // tri décroissant
      for (int j = i + 1; j < na; j++)
        if (acc[j].usd > acc[i].usd) { Acc t = acc[i]; acc[i] = acc[j]; acc[j] = t; }
    for (int i = 0; i < na; i++) {
      if (acc[i].usd < 0.005f) continue;
      if (d.nModels < MAX_MODELS) {
        snprintf(d.model[d.nModels], sizeof(d.model[0]), "%s", acc[i].name);
        d.modelUsd[d.nModels++] = acc[i].usd;
      } else {
        d.otherUsd += acc[i].usd;
      }
    }
  }

  // 2. coût par jour sur 30 jours (le dernier jour est aujourd'hui, partiel)
  {
    long today = daysFromCivil(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    time_t from = (time_t)((today - (MAX_DAYS - 1)) * 86400L), to = (time_t)((today + 1) * 86400L);
    JsonDocument filter;
    filter["data"][0]["starting_at"] = true;
    filter["data"][0]["cost"]["amount"] = true;
    JsonDocument doc;
    int code = apiJson("/v1/organizations/analytics/cost?starting_at=" + isoDay(from) + "&ending_at=" + isoDay(to) +
                       "&time_bucket_width=1d&order_by=time&order=asc&limit=1000", doc, filter);
    if (code != 200) return code;
    d.nDays = MAX_DAYS;
    for (JsonObjectConst row : doc["data"].as<JsonArrayConst>()) {
      long day = (long)(parseIso(row["starting_at"] | "") / 86400L);
      int idx = (int)(day - (today - (MAX_DAYS - 1)));
      if (idx >= 0 && idx < MAX_DAYS) d.dayUsd[idx] += cents(row["cost"]["amount"]);
    }
    struct tm f, l;
    gmtime_r(&from, &f);
    time_t last = to - 86400;
    gmtime_r(&last, &l);
    snprintf(d.firstDay, sizeof(d.firstDay), "%02d/%02d", f.tm_mday, f.tm_mon + 1);
    snprintf(d.lastDay, sizeof(d.lastDay), "%02d/%02d", l.tm_mday, l.tm_mon + 1);
  }

  // 3. tokens et requêtes du mois (une seule ligne sans regroupement)
  {
    JsonDocument filter;
    filter["data"][0]["usage"]["total_tokens"] = true;
    filter["data"][0]["usage"]["requests"] = true;
    JsonDocument doc;
    if (apiJson(String("/v1/organizations/analytics/usage?starting_at=") + monthStart + "&limit=10", doc, filter) == 200) {
      for (JsonObjectConst row : doc["data"].as<JsonArrayConst>()) {
        d.tokens += row["usage"]["total_tokens"] | 0.0;
        d.requests += row["usage"]["requests"] | 0u;
        d.hasUsage = true;
      }
    }
  }

  // estimation fin de mois : rythme jusqu'à la fin des données complètes
  time_t mStart = parseIso(monthStart);
  time_t end = refreshed > mStart ? refreshed : now;
  float elapsed = fmaxf((end - mStart) / 86400.0f, 1.0f);
  int dim = (int)(daysFromCivil(g.tm_mon == 11 ? g.tm_year + 1901 : g.tm_year + 1900, g.tm_mon == 11 ? 1 : g.tm_mon + 2, 1) -
                  daysFromCivil(g.tm_year + 1900, g.tm_mon + 1, 1));
  d.projectionUsd = d.monthUsd / elapsed * dim;
  if (refreshed) {
    struct tm lt;
    localtime_r(&refreshed, &lt);
    snprintf(d.refreshed, sizeof(d.refreshed), "%02d/%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
  }
  data = d;
  UiLock l;
  ui_set_data(d);
  ui_show_main();
  return 200;
}

// ------------------------------------------------------------------ pages web
static String scanOptions;

static void handleRoot() {
  String page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Suivi Claude</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b0e14;color:#eee;max-width:480px;margin:auto;padding:24px}"
    "input,button{width:100%;padding:11px;margin:4px 0 14px;border-radius:8px;border:1px solid #333;"
    "background:#151a23;color:#eee;font-size:15px;box-sizing:border-box;font-family:inherit}"
    "button{background:#d97757;border:0;font-weight:600;font-size:16px}label{font-size:14px;color:#bbb}"
    "small{color:#7d8696;display:block;margin:-8px 0 14px}h3{margin-top:26px}"
    "</style></head><body><h2>Suivi Claude &ndash; réglages</h2><form method='POST' action='/save'>";
  if (setupMode) {
    page += "<h3>Wi-Fi</h3><label>Réseau</label><input name='s' list='nets' required autocomplete='off' value='" +
            htmlEscape(savedSsid) + "'><datalist id='nets'>" + scanOptions + "</datalist>"
            "<label>Mot de passe</label><input name='p' type='password'>";
  }
  page += "<h3>Organisation Claude</h3><label>Clé Admin API (sk-ant-admin…)</label>"
          "<input name='key' type='password' autocomplete='off' placeholder='" +
          String(adminKey.length() ? "enregistrée — laisser vide pour la garder" : "à saisir") + "'>"
          "<small>Console Claude &rarr; Settings &rarr; Admin keys. Cette clé donne tous les droits sur "
          "l'organisation : elle reste dans le module et n'est envoyée qu'à api.anthropic.com (certificat vérifié).</small>"
          "<label>Budget mensuel en $ (0 = aucun)</label><input name='budget' type='number' min='0' step='1' value='" +
          String((int)budgetUsd) + "'>"
          "<button type='submit'>Enregistrer et redémarrer</button></form>";
  if (adminKey.length())
    page += "<form method='POST' action='/forget' onsubmit=\"return confirm('Effacer la clé du module ?')\">"
            "<button style='background:#ea3943'>Effacer la clé du module</button></form>";
  if (!setupMode)
    page += "<form method='POST' action='/wifi' onsubmit=\"return confirm('Oublier le Wi-Fi ?')\">"
            "<button style='background:#333'>Changer de réseau Wi-Fi</button></form>";
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
  if (web.hasArg("key") && web.arg("key").length()) {
    String k = web.arg("key");
    k.trim();
    prefs.putString("key", k);
  }
  if (web.hasArg("budget")) prefs.putFloat("budget", web.arg("budget").toFloat());
  sendDone("Le module redémarre...");
  delay(1500);
  ESP.restart();
}

static void handleForget() {
  prefs.remove("key");
  sendDone("Clé effacée. Le module redémarre...");
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
  web.on("/forget", HTTP_POST, handleForget);
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
                  "3. Choisissez votre Wi-Fi et saisissez la cle Admin API");
}

static bool connectWifi() {
  if (!savedSsid.length()) return false;
  {
    UiLock l;
    ui_show_message("Connexion Wi-Fi...", "");
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
  improv_begin("Suivi Claude", "1.0.0", "http://" MDNS_NAME ".local");
  improv_set_line_hook(screenshot_handle_line);
  improv_delay(1500);
  Serial.println("[boot] Suivi Claude");
  i2c_master_Init();
  power_init();
  lvgl_port_init();
  screenshot_begin(lvgl_lock, lvgl_unlock);
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  prefs.begin("claude", false);
  wifi_shared_load(savedSsid, savedPass);
  forcePortal = prefs.getBool("portal", false);
  if (forcePortal) prefs.remove("portal");
  adminKey = prefs.isKey("key") ? prefs.getString("key", "") : String();
  budgetUsd = prefs.getFloat("budget", 0);
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
  tls.setCACert(ANTHROPIC_ROOT_CA);
  String ip = WiFi.localIP().toString();
  Serial.printf("[wifi] connecte, IP %s\n", ip.c_str());
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  setupWebRoutes();
  web.begin();

  String info = "Cle Admin API et budget :\n"
                "   http://" MDNS_NAME ".local   ou   http://" + ip + "\n\n"
                "Donnees : Usage and Cost Analytics API, mises a jour\n"
                "par Anthropic avec 4 a 24 h de retard. Lecture toutes les 15 min.";
  UiLock l;
  ui_set_info(info.c_str());
  if (!adminKey.length()) {
    String m = "Saisissez une cle Admin API (sk-ant-admin...) sur\nhttp://" MDNS_NAME ".local  ou  http://" + ip;
    ui_show_message("Cle Admin API", m.c_str());
  } else {
    ui_show_message("Suivi Claude", "Lecture des depenses...");
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
  if (!adminKey.length()) { delay(20); return; }
  if (WiFi.status() != WL_CONNECTED) {
    status("Wi-Fi hors ligne", false);
    delay(500);
    return;
  }
  if (time(NULL) < 1700000000) { delay(100); return; }  // attendre l'heure NTP (dates des requêtes)

  static uint32_t nextPoll = 0;
  static bool stopped = false;  // clé refusée : on attend une nouvelle clé
  if (stopped && !g_reqRefresh) { delay(20); return; }
  if (g_reqRefresh || !nextPoll || (int32_t)(millis() - nextPoll) >= 0) {
    g_reqRefresh = false;
    stopped = false;
    int code = refreshData();
    nextPoll = millis() + (code == 200 ? POLL_MS : RETRY_MS);
    Serial.printf("[api] refresh %d, heap %u\n", code, ESP.getFreeHeap());
    if (code != 200) {
      const char *msg = code == 401 ? "Cle refusee"
                      : code == 403 ? "Cle sans droits admin"
                      : code == 404 ? "API non disponible pour l'organisation"
                      : code == 429 ? "Trop de requetes, nouvel essai"
                      : code == -2 ? "Reponse illisible"
                      : code < 0 ? "Connexion a l'API impossible"
                      : "Erreur de l'API";
      status(msg, false);
      if (code == 401 || code == 403 || code == 404) {
        stopped = true;
        UiLock l;
        String m = String(msg) + ".\nVerifiez la cle (Admin API, organisation Console) sur http://" MDNS_NAME ".local";
        ui_show_message("Acces refuse", m.c_str());
      }
    }
  }
  delay(20);
}
