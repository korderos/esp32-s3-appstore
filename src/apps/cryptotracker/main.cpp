// Crypto tracker — Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B 172x640, QSPI)
// Données : API publique Binance (sans clé). Wi-Fi configuré via portail captif.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>

#include "user_config.h"
#include "lvgl_port.h"
#include "i2c_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "coins.h"
#include "ui.h"
// Pas d'identifiants Wi-Fi dans le firmware publié : le réseau se configure
// uniquement via le portail captif et reste enregistré dans la NVS du module.

#define AP_NAME            "CryptoTracker-Setup"
#define TICKER_PERIOD_MS   15000
#define CHART_PERIOD_MS    60000
#define TZ_PARIS           "CET-1CEST,M3.5.0,M10.5.0/3"

static Preferences prefs;
static WiFiClientSecure tls;
static WebServer web(80);
static DNSServer dns;
static bool setupMode = false;
static bool forcePortal = false;
static String savedSsid, savedPass;

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
  http.setReuse(true);
  http.setTimeout(8000);
  if (!http.begin(tls, url)) return false;
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK;
  if (ok) out = http.getString();
  else Serial.printf("HTTP %d pour %s\n", code, url.c_str());
  http.end();
  return ok;
}

// ------------------------------------------------------------------ données Binance
static bool fetchTickers() {
  String url = "https://api.binance.com/api/v3/ticker/24hr?symbols=%5B";
  for (int i = 0; i < NUM_COINS; i++) {
    if (i) url += ",";
    url += "%22";
    url += COINS[i].sym;
    url += QUOTE "%22";
  }
  url += "%5D";

  String body;
  if (!httpGet(url, body)) return false;

  JsonDocument filter;
  filter[0]["symbol"] = true;
  filter[0]["lastPrice"] = true;
  filter[0]["priceChangePercent"] = true;
  filter[0]["highPrice"] = true;
  filter[0]["lowPrice"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  UiLock l;
  for (JsonObject o : doc.as<JsonArray>()) {
    String sym = o["symbol"] | "";
    for (int i = 0; i < NUM_COINS; i++) {
      if (sym == String(COINS[i].sym) + QUOTE) {
        ui_set_ticker(i, atof(o["lastPrice"] | "0"), atof(o["priceChangePercent"] | "0"),
                      atof(o["highPrice"] | "0"), atof(o["lowPrice"] | "0"));
      }
    }
  }
  return true;
}

static bool fetchChart(int coin, int tf) {
  String url = String("https://api.binance.com/api/v3/klines?symbol=") + COINS[coin].sym + QUOTE +
               "&interval=" + TFS[tf].interval + "&limit=" + TFS[tf].limit;
  String body;
  if (!httpGet(url, body)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  static double closes[MAX_POINTS];
  int n = 0;
  for (JsonArray k : doc.as<JsonArray>()) {
    if (n >= MAX_POINTS) break;
    closes[n++] = atof(k[4] | "0");  // index 4 = prix de clôture
  }
  UiLock l;
  ui_set_chart(coin, tf, closes, n);
  return true;
}

// ------------------------------------------------------------------ portail Wi-Fi
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
  String page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CryptoTracker</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b0e14;color:#eee;max-width:420px;margin:auto;padding:24px}"
    "input,select,button{width:100%;padding:12px;margin:6px 0 16px;border-radius:8px;border:1px solid #333;"
    "background:#151a23;color:#eee;font-size:16px;box-sizing:border-box}"
    "button{background:#3b82f6;border:0;font-weight:600}</style></head><body>"
    "<h2>CryptoTracker &ndash; Wi-Fi</h2><form method='POST' action='/save'>"
    "<label>Réseau</label><input name='s' list='nets' required autocomplete='off'>"
    "<datalist id='nets'>" + scanOptions + "</datalist>"
    "<label>Mot de passe</label><input name='p' type='password'>"
    "<button type='submit'>Enregistrer et redémarrer</button></form></body></html>";
  web.send(200, "text/html; charset=utf-8", page);
}

static void handleSave() {
  prefs.putString("ssid", web.arg("s"));
  prefs.putString("pass", web.arg("p"));
  web.send(200, "text/html; charset=utf-8",
           "<meta charset='utf-8'><body style='font-family:sans-serif;background:#0b0e14;color:#eee;padding:24px'>"
           "<h2>Enregistré</h2><p>Le tracker redémarre et se connecte...</p>");
  delay(1500);
  ESP.restart();
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
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([] {
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();

  UiLock l;
  ui_show_message("Configuration Wi-Fi",
                  "1. Connectez-vous au reseau Wi-Fi \"" AP_NAME "\"\n"
                  "2. Ouvrez http://192.168.4.1 (la page s'ouvre souvent seule)\n"
                  "3. Choisissez votre Wi-Fi et saisissez le mot de passe");
}

static bool connectWifi() {
  if (!savedSsid.length()) return false;
  {
    UiLock l;
    String m = "Reseau : " + savedSsid;
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
// L'écran est utilisé en paysage ; l'accéléromètre indique si la carte est tenue
// "à l'endroit" ou retournée de 180°, et on retourne l'affichage + le tactile en conséquence.
// Si l'orientation est inversée, changez le signe de IMU_AXIS_SIGN.
#define IMU_AXIS       1     // 0 = X, 1 = Y : axe IMU parallèle au petit côté de l'écran
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
  Serial.println("[imu] QMI8658 ok");
}

static void imu_poll() {
  static uint32_t last = 0, lastLog = 0;
  static int votes = 0;
  if (!imuOk || millis() - last < 200) return;
  last = millis();
  uint8_t b[6];
  if (i2c_read_buff(imu_dev_handle, 0x35, b, 6) != ESP_OK) return;
  int16_t ax = (int16_t)(b[1] << 8 | b[0]);
  int16_t ay = (int16_t)(b[3] << 8 | b[2]);
  int16_t az = (int16_t)(b[5] << 8 | b[4]);
  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial.printf("[imu] ax=%d ay=%d az=%d flip=%d\n", ax, ay, az, lvgl_get_flip());
  }
  // ±4 g -> 8192 LSB/g. On ne décide que si la carte est nettement inclinée (> ~0,5 g)
  int a = (IMU_AXIS == 0 ? ax : ay) * IMU_AXIS_SIGN;
  int other = (IMU_AXIS == 0 ? ay : ax);
  // gravité nettement le long du petit côté (> 0,6 g et dominante) : sinon (à plat, portrait) on ne touche à rien
  if (abs(a) < 4900 || abs(a) < 2 * abs(other) || abs(a) < abs(az)) { votes = 0; return; }
  bool wantFlip = a < 0;
  if (wantFlip == lvgl_get_flip()) { votes = 0; return; }
  if (++votes < 4) return;  // ~800 ms stable avant de basculer
  votes = 0;
  UiLock l;
  lvgl_set_flip(wantFlip);
}

// ------------------------------------------------------------------ batterie
// Tension batterie sur GPIO4 (ADC1 canal 3) via un pont diviseur /3.
// L'expandeur TCA9554 (0x20) : EXIO6 = 1 maintient l'alimentation batterie.
// Ne pas toucher EXIO1 : le mettre à 0 coupe l'écran (les démos Waveshare sans écran le font).
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
  uint8_t out = 0xFF;                // toutes les sorties à 1 (EXIO6 haut)
  uint8_t dir = 0xFF & ~(1 << 6);    // seul EXIO6 en sortie, le reste en entrée (état d'origine)
  if (i2c_write_buff(tca_dev, 0x01, &out, 1) != ESP_OK || i2c_write_buff(tca_dev, 0x03, &dir, 1) != ESP_OK)
    Serial.println("[bat] TCA9554 absent");
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}

static float battery_volts() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  return mv / 16.0f * 3.0f / 1000.0f;
}

// courbe de décharge typique d'une cellule Li-ion
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
  static uint32_t last = 0, lastLog = 0;
  static float filt = -1;
  if (last && millis() - last < 2000) return;
  last = millis();
  float v = battery_volts();
  filt = filt < 0 ? v : filt * 0.8f + v * 0.2f;  // lissage
  bool usb = Serial.isPlugged();
  int pct = battery_percent(filt);
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    Serial.printf("[bat] %.3f V (lisse %.3f) -> %d %% usb=%d\n", v, filt, pct, usb);
  }
  UiLock l;
  ui_set_battery(pct, usb);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[boot] CryptoTracker");
  i2c_master_Init();
  power_init();
  lvgl_port_init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);  // luminosité max (PWM inversé)

  prefs.begin("tracker", false);
  // Wi-Fi : identifiants saisis via le portail (vide au premier démarrage -> portail)
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  forcePortal = prefs.getBool("portal", false);
  if (forcePortal) prefs.remove("portal");
  imu_init();
  {
    UiLock l;
    ui_init(prefs.getInt("coin", 0), prefs.getInt("tf", 1));
  }

  Serial.printf("[boot] UI ok, PSRAM libre %u, heap %u\n", ESP.getFreePsram(), ESP.getFreeHeap());
  if (forcePortal || !connectWifi()) {
    Serial.println("[wifi] pas de connexion -> portail " AP_NAME);
    startSetupPortal();
    return;
  }
  configTzTime(TZ_PARIS, "pool.ntp.org", "time.google.com");
  tls.setInsecure();  // données publiques en lecture seule ; pas de magasin de certificats
  Serial.printf("[wifi] connecte, IP %s\n", WiFi.localIP().toString().c_str());
  UiLock l;
  ui_show_main();
  ui_set_status("Connecte", true);
}

void loop() {
  if (g_reqWifiReset) {
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.putBool("portal", true);  // au prochain démarrage : portail de configuration
    delay(200);
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

  static uint32_t lastTicker = 0, lastChart = 0;
  static int savedCoin = -1, savedTf = -1;
  uint32_t now = millis();

  // mémoriser la sélection
  if (savedCoin != g_coin) { savedCoin = g_coin; prefs.putInt("coin", savedCoin); }
  if (savedTf != g_tf) { savedTf = g_tf; prefs.putInt("tf", savedTf); }

  if (WiFi.status() != WL_CONNECTED) {
    status("Hors ligne", false);
    delay(1000);
    return;
  }

  bool okAll = true, did = false;
  if (g_reqChart || lastChart == 0 || now - lastChart > CHART_PERIOD_MS) {
    g_reqChart = false;
    int c = g_coin, t = g_tf;
    bool ok = fetchChart(c, t);
    okAll &= ok;
    lastChart = ok ? now : now - CHART_PERIOD_MS + 5000;  // en cas d'échec, réessai dans 5 s
    did = true;
  }
  if (lastTicker == 0 || now - lastTicker > TICKER_PERIOD_MS) {
    okAll &= fetchTickers();
    lastTicker = now;
    did = true;
  }

  if (did) {
    Serial.printf("[data] coin=%s tf=%s ok=%d heap=%u\n", COINS[g_coin].sym, TFS[g_tf].label, okAll, ESP.getFreeHeap());
    char buf[32];
    struct tm ti;
    if (!okAll) snprintf(buf, sizeof(buf), "Erreur API");
    else if (getLocalTime(&ti, 10)) strftime(buf, sizeof(buf), "MAJ %H:%M:%S", &ti);
    else snprintf(buf, sizeof(buf), "MAJ OK");
    status(buf, okAll);
  }
  delay(20);
}
