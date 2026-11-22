// Météo — Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B 172x640, QSPI), paysage
// Données : API publique Open-Meteo (sans clé), géocodage Open-Meteo.
// Wi-Fi et liste des villes configurés via une page web (portail captif au premier
// démarrage, puis http://meteo.local ou l'adresse IP affichée dans les réglages).
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
#include "weather.h"
#include "ui.h"

#define AP_NAME          "Meteo-Setup"
#define MDNS_NAME        "meteo"
#define WX_PERIOD_MS     (10UL * 60 * 1000)  // rafraîchissement de chaque ville
#define WX_RETRY_MS      30000
#define DEFAULT_CITIES   "Paris\nLyon\nMarseille\nLille"

static Preferences prefs;
static WiFiClientSecure tls;
static WebServer web(80);
static DNSServer dns;
static bool setupMode = false;
static bool forcePortal = false;
static String savedSsid, savedPass;

struct City {
  String query;          // saisie de l'utilisateur, ex. "Lyon" ou "Springfield, US"
  String name, region;   // résultat du géocodage (ASCII)
  float lat = 0, lon = 0;
  bool geo = false, geoFail = false;
  uint32_t lastOk = 0, nextTry = 0;
};
static City cities[MAX_CITIES];
static int nCities = 0;

// ------------------------------------------------------------------ utilitaires
struct UiLock {
  UiLock() { lvgl_lock(-1); }
  ~UiLock() { lvgl_unlock(); }
};

static void status(const char *txt, bool ok) {
  UiLock l;
  ui_set_status(txt, ok);
}

static bool httpGet(const String &url, String &out) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(tls, url)) return false;
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK;
  if (ok) out = http.getString();
  else Serial.printf("HTTP %d pour %s\n", code, url.c_str());
  http.end();
  return ok;
}

static String urlEncode(const String &s) {
  String o;
  char b[4];
  for (uint8_t c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
    else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
  }
  return o;
}

// Les polices LVGL intégrées n'ont pas les accents : on translittère l'UTF-8 en ASCII
// (en gardant le symbole degré, présent dans la police).
static String toAscii(const String &in) {
  static const char *LAT1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  String o;
  const uint8_t *p = (const uint8_t *)in.c_str();
  while (*p) {
    uint8_t c = *p;
    if (c < 0x80) { o += (char)c; p++; continue; }
    uint32_t cp;
    int len;
    if ((c & 0xE0) == 0xC0 && p[1]) { cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
    else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
    else { p++; continue; }
    p += len;
    if (cp == 0xB0) o += "\xC2\xB0";
    else if (cp >= 0xC0 && cp <= 0xFF) o += LAT1[cp - 0xC0];
    else if (cp == 0x152) o += "OE";
    else if (cp == 0x153) o += "oe";
    else if (cp == 0x160 || cp == 0x161) o += (cp == 0x160 ? 'S' : 's');
    else if (cp == 0x17D || cp == 0x17E) o += (cp == 0x17D ? 'Z' : 'z');
    else if (cp == 0x2019 || cp == 0x2018) o += '\'';
    else if (cp == 0x2013 || cp == 0x2014) o += '-';
  }
  return o;
}

// "2026-11-22" -> jour de la semaine (0 = dimanche), algorithme de Sakamoto
static int weekday(const char *s) {
  int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 1 || m > 12) return 0;
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// ------------------------------------------------------------------ villes
static void parseCities(const String &src) {
  nCities = 0;
  String cur;
  auto push = [&]() {
    cur.trim();
    if (cur.length() && nCities < MAX_CITIES) cities[nCities++].query = cur;
    cur = "";
  };
  for (size_t i = 0; i < src.length(); i++) {
    char c = src[i];
    if (c == '\n' || c == '\r' || c == ';') push();
    else cur += c;
  }
  push();
  if (!nCities) cities[nCities++].query = "Paris";
}

// cache du géocodage dans la NVS : une ligne "requete\tnom\tregion\tlat\tlon" par ville
static void loadGeoCache() {
  String cache = prefs.getString("geo", "");
  int start = 0;
  while (start < (int)cache.length()) {
    int end = cache.indexOf('\n', start);
    if (end < 0) end = cache.length();
    String line = cache.substring(start, end);
    start = end + 1;
    int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1), t3 = line.indexOf('\t', t2 + 1), t4 = line.indexOf('\t', t3 + 1);
    if (t1 < 0 || t2 < 0 || t3 < 0 || t4 < 0) continue;
    String q = line.substring(0, t1);
    for (int i = 0; i < nCities; i++) {
      if (cities[i].query == q && !cities[i].geo) {
        cities[i].name = line.substring(t1 + 1, t2);
        cities[i].region = line.substring(t2 + 1, t3);
        cities[i].lat = line.substring(t3 + 1, t4).toFloat();
        cities[i].lon = line.substring(t4 + 1).toFloat();
        cities[i].geo = true;
      }
    }
  }
}

static void saveGeoCache() {
  String cache;
  for (int i = 0; i < nCities; i++) {
    if (!cities[i].geo) continue;
    cache += cities[i].query + "\t" + cities[i].name + "\t" + cities[i].region + "\t" +
             String(cities[i].lat, 4) + "\t" + String(cities[i].lon, 4) + "\n";
  }
  prefs.putString("geo", cache);
}

static bool geocode(City &c) {
  String name = c.query, cc;
  int comma = name.lastIndexOf(',');
  if (comma > 0) {  // "Ville, CC" : filtre sur le code pays ISO à 2 lettres
    String tail = name.substring(comma + 1);
    tail.trim();
    if (tail.length() == 2) { cc = tail; cc.toUpperCase(); name = name.substring(0, comma); name.trim(); }
  }
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=fr&format=json&name=" + urlEncode(name);
  if (cc.length()) url += "&countryCode=" + cc;
  String body;
  if (!httpGet(url, body)) return false;  // erreur réseau : on réessaiera
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject r = doc["results"][0];
  if (r.isNull()) { c.geoFail = true; return false; }
  c.name = toAscii(r["name"] | c.query.c_str());
  String admin = r["admin1"] | "", country = r["country"] | "";
  c.region = toAscii(admin.length() && admin != c.name ? admin + ", " + country : country);
  c.lat = r["latitude"] | 0.0f;
  c.lon = r["longitude"] | 0.0f;
  c.geo = true;
  return true;
}

// ------------------------------------------------------------------ météo Open-Meteo
static bool fetchWeather(int idx) {
  City &c = cities[idx];
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(c.lat, 4) + "&longitude=" + String(c.lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,"
               "cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
               "&hourly=temperature_2m,precipitation_probability"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,"
               "precipitation_sum,precipitation_probability_max"
               "&timezone=auto&forecast_days=7&forecast_hours=24&wind_speed_unit=kmh";
  String body;
  if (!httpGet(url, body)) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) { Serial.printf("[wx] JSON : %s\n", err.c_str()); return false; }

  static WxData w;
  memset(&w, 0, sizeof(w));
  JsonObject cur = doc["current"];
  w.temp = cur["temperature_2m"] | 0.0f;
  w.feels = cur["apparent_temperature"] | w.temp;
  w.hum = cur["relative_humidity_2m"] | 0;
  w.isDay = (cur["is_day"] | 1) != 0;
  w.precip = cur["precipitation"] | 0.0f;
  w.code = cur["weather_code"] | 0;
  w.cloud = cur["cloud_cover"] | 0;
  w.pressure = cur["pressure_msl"] | 0.0f;
  w.wind = cur["wind_speed_10m"] | 0.0f;
  w.windDir = cur["wind_direction_10m"] | 0;
  w.gust = cur["wind_gusts_10m"] | 0.0f;
  w.utcOffset = doc["utc_offset_seconds"] | 0;

  JsonObject h = doc["hourly"];
  JsonArray ht = h["temperature_2m"], hp = h["precipitation_probability"];
  const char *t0 = h["time"][0] | "";
  w.startHour = strlen(t0) >= 13 ? atoi(t0 + 11) : 0;
  for (int i = 0; i < WX_HOURS && i < (int)ht.size(); i++) {
    w.hTemp[i] = ht[i] | 0.0f;
    w.hPop[i] = hp[i] | 0;
    w.nh = i + 1;
  }

  JsonObject d = doc["daily"];
  JsonArray dt = d["time"];
  for (int i = 0; i < WX_DAYS && i < (int)dt.size(); i++) {
    WxDay &x = w.d[i];
    x.wday = weekday(dt[i] | "2000-01-01");
    x.code = d["weather_code"][i] | 0;
    x.tmax = d["temperature_2m_max"][i] | 0.0f;
    x.tmin = d["temperature_2m_min"][i] | 0.0f;
    x.uv = d["uv_index_max"][i] | 0.0f;
    x.rain = d["precipitation_sum"][i] | 0.0f;
    x.pop = d["precipitation_probability_max"][i] | 0;
    const char *sr = d["sunrise"][i] | "", *ss = d["sunset"][i] | "";
    snprintf(x.rise, sizeof(x.rise), "%s", strlen(sr) >= 16 ? sr + 11 : "--");
    snprintf(x.set, sizeof(x.set), "%s", strlen(ss) >= 16 ? ss + 11 : "--");
    w.nd = i + 1;
  }

  UiLock l;
  ui_set_weather(idx, w);
  return true;
}

// ------------------------------------------------------------------ pages web
static String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static String scanOptions;

static void handleRoot() {
  String list;
  for (int i = 0; i < nCities; i++) list += htmlEscape(cities[i].query) + "\n";
  String page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Météo</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b0e14;color:#eee;max-width:440px;margin:auto;padding:24px}"
    "input,textarea,button{width:100%;padding:12px;margin:6px 0 16px;border-radius:8px;border:1px solid #333;"
    "background:#151a23;color:#eee;font-size:16px;box-sizing:border-box;font-family:inherit}"
    "button{background:#3b82f6;border:0;font-weight:600}small{color:#7d8696;display:block;margin-top:-10px;margin-bottom:16px}"
    "h3{margin-top:28px}</style></head><body><h2>Météo &ndash; réglages</h2>"
    "<form method='POST' action='/save'>";
  if (setupMode) {
    page += "<h3>Wi-Fi</h3><label>Réseau</label><input name='s' list='nets' required autocomplete='off' value='" +
            htmlEscape(savedSsid) + "'><datalist id='nets'>" + scanOptions + "</datalist>"
            "<label>Mot de passe</label><input name='p' type='password'>";
  }
  page += "<h3>Villes</h3><label>Une ville par ligne (" + String(MAX_CITIES) + " max)</label>"
          "<textarea name='c' rows='8'>" + list + "</textarea>"
          "<small>Pour lever une ambiguïté, ajoutez le code pays : « Springfield, US ».</small>"
          "<button type='submit'>Enregistrer et redémarrer</button></form>";
  if (!setupMode) {
    page += "<form method='POST' action='/wifi' onsubmit=\"return confirm('Oublier le Wi-Fi ?')\">"
            "<button style='background:#ea3943'>Changer de réseau Wi-Fi</button></form>";
  }
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
  if (web.hasArg("s") && web.arg("s").length()) {
    prefs.putString("ssid", web.arg("s"));
    prefs.putString("pass", web.arg("p"));
  }
  if (web.hasArg("c")) {
    String c = web.arg("c");
    c.trim();
    prefs.putString("cities", c.length() ? c : String(DEFAULT_CITIES));
    prefs.putInt("city", 0);
  }
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
  // si des identifiants existent, on continue d'essayer en arrière-plan (box redémarrée, etc.)
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
                  "3. Choisissez votre Wi-Fi et la liste de vos villes");
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
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ------------------------------------------------------------------ orientation (IMU QMI8658)
// Même logique que CryptoTracker : l'affichage et le tactile se retournent de 180°
// selon le sens dans lequel la carte est tenue.
#define IMU_AXIS       1
#define IMU_AXIS_SIGN  -1
static bool imuOk = false;

static void imu_init() {
  uint8_t id = 0;
  if (i2c_read_buff(imu_dev_handle, 0x00, &id, 1) != ESP_OK || id != 0x05) {
    Serial.printf("[imu] QMI8658 absent (id=0x%02X)\n", id);
    return;
  }
  uint8_t v;
  v = 0x40; i2c_write_buff(imu_dev_handle, 0x02, &v, 1);  // CTRL1 : auto-incrément d'adresse
  v = 0x15; i2c_write_buff(imu_dev_handle, 0x03, &v, 1);  // CTRL2 : ±4 g, ODR ~235 Hz
  v = 0x01; i2c_write_buff(imu_dev_handle, 0x08, &v, 1);  // CTRL7 : accéléromètre ON
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
// Tension sur GPIO4 via un pont /3 ; EXIO6 du TCA9554 (0x20) maintient l'alimentation batterie.
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
  uint8_t out = 0xFF;
  uint8_t dir = 0xFF & ~(1 << 6);
  if (i2c_write_buff(tca_dev, 0x01, &out, 1) != ESP_OK || i2c_write_buff(tca_dev, 0x03, &dir, 1) != ESP_OK)
    Serial.println("[bat] TCA9554 absent");
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
  delay(1500);
  Serial.println("[boot] Meteo");
  i2c_master_Init();
  power_init();
  lvgl_port_init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  prefs.begin("meteo", false);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  forcePortal = prefs.getBool("portal", false);
  if (forcePortal) prefs.remove("portal");
  parseCities(prefs.getString("cities", DEFAULT_CITIES));
  loadGeoCache();
  imu_init();
  {
    String shown[MAX_CITIES];
    const char *names[MAX_CITIES];
    for (int i = 0; i < nCities; i++) {
      shown[i] = toAscii(cities[i].geo ? cities[i].name : cities[i].query);
      names[i] = shown[i].c_str();
    }
    UiLock l;
    ui_init(nCities, names, prefs.getInt("city", 0), prefs.getInt("view", 0));
    for (int i = 0; i < nCities; i++)
      if (cities[i].geo) ui_set_city(i, cities[i].name.c_str(), cities[i].region.c_str());
  }

  if (forcePortal || !connectWifi()) {
    Serial.println("[wifi] pas de connexion -> portail " AP_NAME);
    startSetupPortal();
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC ; chaque ville a son décalage
  tls.setInsecure();  // données publiques en lecture seule ; pas de magasin de certificats
  String ip = WiFi.localIP().toString();
  Serial.printf("[wifi] connecte, IP %s\n", ip.c_str());
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  setupWebRoutes();
  web.begin();

  String info = "Modifier les villes depuis un telephone ou un PC du meme reseau :\n"
                "   http://" MDNS_NAME ".local   ou   http://" + ip + "\n\n"
                "Balayez l'ecran vers la gauche ou la droite pour changer de ville.\n"
                "Donnees : Open-Meteo, mises a jour toutes les 10 min.";
  UiLock l;
  ui_set_info(info.c_str());
  ui_show_main();
  ui_set_status("Connecte", true);
}

// Fait avancer une ville d'une étape (géocodage puis météo) ; renvoie true si un appel réseau a eu lieu.
static bool serviceCity(int i, bool force) {
  City &c = cities[i];
  uint32_t now = millis();
  if (c.geoFail) return false;
  if (!force && c.nextTry && (int32_t)(now - c.nextTry) < 0) return false;
  if (!c.geo) {
    if (geocode(c)) {
      saveGeoCache();
      UiLock l;
      ui_set_city(i, c.name.c_str(), c.region.c_str());
    } else {
      c.nextTry = now + WX_RETRY_MS;
      UiLock l;
      ui_set_city_error(i, c.geoFail ? "Ville introuvable" : "Erreur de geocodage");
      return true;
    }
  }
  bool ok = fetchWeather(i);
  c.nextTry = millis() + (ok ? WX_PERIOD_MS : WX_RETRY_MS);
  if (ok) c.lastOk = millis();
  Serial.printf("[wx] %s ok=%d heap=%u\n", c.name.c_str(), ok, ESP.getFreeHeap());
  if (!ok && !c.lastOk) { UiLock l; ui_set_city_error(i, "Erreur API meteo"); }
  status(ok ? "MAJ OK" : "Erreur API", ok);
  return true;
}

void loop() {
  if (g_reqWifiReset) {
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.putBool("portal", true);
    delay(300);
    ESP.restart();
  }
  imu_poll();
  battery_poll();

  if (setupMode) {
    dns.processNextRequest();
    web.handleClient();
    if (WiFi.status() == WL_CONNECTED) ESP.restart();  // le réseau sauvegardé est revenu
    delay(5);
    return;
  }
  web.handleClient();

  static int savedCity = -1, savedView = -1;
  if (savedCity != g_city) { savedCity = g_city; prefs.putInt("city", savedCity); }
  if (savedView != g_view) { savedView = g_view; prefs.putInt("view", savedView); }

  if (WiFi.status() != WL_CONNECTED) {
    status("Hors ligne", false);
    delay(500);
    return;
  }

  // priorité à la ville affichée, puis les autres à tour de rôle
  bool force = g_reqRefresh;
  g_reqRefresh = false;
  if (!serviceCity(g_city, force)) {
    for (int k = 1; k < nCities; k++)
      if (serviceCity((g_city + k) % nCities, false)) break;
  }
  delay(20);
}
