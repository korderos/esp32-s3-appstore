#pragma once
// Wi-Fi partagé entre toutes les applis de l'appstore + protocole Improv Wi-Fi (série).
//
// - Identifiants rangés dans la NVS, espace "wifi" : l'installation d'une appli par
//   l'appstore n'efface pas la NVS, donc le réseau saisi une fois sert à toutes.
// - Improv (https://www.improv-wifi.com/serial/) : à la fin d'une installation, la
//   page de l'appstore (ESP Web Tools) propose de saisir le Wi-Fi et l'envoie par USB.
//
// Usage (à inclure dans un seul fichier .cpp par appli) :
//   wifi_shared_load(ssid, pass, "ancien_namespace");
//   improv_begin("Meteo", "1.0.0", "http://meteo.local");
//   improv_poll();   // souvent : setup(), attentes de connexion, loop()
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#define WIFI_SHARED_NS "wifi"

// Lit le réseau partagé. S'il est vide, reprend celui qu'une ancienne version de l'appli
// rangeait dans son propre espace NVS (legacyNs), et le partage.
static inline bool wifi_shared_load(String &ssid, String &pass, const char *legacyNs = nullptr) {
  Preferences p;
  p.begin(WIFI_SHARED_NS, true);
  ssid = p.isKey("ssid") ? p.getString("ssid", "") : String();
  pass = p.isKey("pass") ? p.getString("pass", "") : String();
  p.end();
  if (!ssid.length() && legacyNs) {
    Preferences old;
    if (old.begin(legacyNs, true)) {
      if (old.isKey("ssid")) ssid = old.getString("ssid", "");
      if (old.isKey("pass")) pass = old.getString("pass", "");
      old.end();
    }
    if (ssid.length()) {
      Preferences w;
      w.begin(WIFI_SHARED_NS, false);
      w.putString("ssid", ssid);
      w.putString("pass", pass);
      w.end();
    }
  }
  return ssid.length() > 0;
}

static inline void wifi_shared_save(const String &ssid, const String &pass) {
  Preferences p;
  p.begin(WIFI_SHARED_NS, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

// Oublie le réseau pour toutes les applis (et dans l'ancien espace de l'appli).
static inline void wifi_shared_forget(const char *legacyNs = nullptr) {
  Preferences p;
  p.begin(WIFI_SHARED_NS, false);
  p.clear();
  p.end();
  if (legacyNs) {
    Preferences old;
    if (old.begin(legacyNs, false)) {
      old.remove("ssid");
      old.remove("pass");
      old.end();
    }
  }
}

// ------------------------------------------------------------------ Improv Wi-Fi série
namespace improv_detail {
enum : uint8_t { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum : uint8_t { S_READY = 2, S_PROVISIONING = 3, S_PROVISIONED = 4 };
enum : uint8_t { E_NONE = 0, E_INVALID = 1, E_UNKNOWN_CMD = 2, E_CONNECT = 3 };
enum : uint8_t { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

static const char *fwName = "App", *fwVersion = "1.0.0";
static String url;
static uint8_t buf[256 + 16];
static int pos = 0;
static void (*lineHook)(const char *) = nullptr;  // lignes texte hors Improv (ex. "SCREENSHOT")
static char line[32];
static int lineLen = 0;

static inline void textByte(uint8_t c) {
  if (c == '\r') return;
  if (c == '\n') {
    line[lineLen] = 0;
    lineLen = 0;
    if (lineHook && line[0]) lineHook(line);
    return;
  }
  if (lineLen < (int)sizeof(line) - 1) line[lineLen++] = c;
}

static inline void send(uint8_t type, const uint8_t *data, uint8_t n) {
  uint8_t hdr[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, n};
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (int i = 0; i < n; i++) sum += data[i];
  Serial.write(hdr, sizeof(hdr));
  if (n) Serial.write(data, n);
  Serial.write(sum);
  Serial.write('\n');
  Serial.flush();
}

static inline void sendState(uint8_t s) { send(T_STATE, &s, 1); }
static inline void sendError(uint8_t e) { send(T_ERROR, &e, 1); }

// résultat RPC : commande, longueur totale, puis chaînes préfixées par leur longueur
static inline void sendResult(uint8_t cmd, const char *const *strs, int n) {
  uint8_t d[250];
  int o = 2;
  for (int i = 0; i < n; i++) {
    int l = strlen(strs[i]);
    if (l > 64) l = 64;
    if (o + 1 + l > (int)sizeof(d)) break;
    d[o++] = l;
    memcpy(d + o, strs[i], l);
    o += l;
  }
  d[0] = cmd;
  d[1] = o - 2;
  send(T_RESULT, d, o);
}

static inline uint8_t currentState() { return WiFi.status() == WL_CONNECTED ? S_PROVISIONED : S_READY; }

static inline void sendUrlResult(uint8_t cmd) {
  if (url.length()) { const char *s[1] = {url.c_str()}; sendResult(cmd, s, 1); }
  else sendResult(cmd, nullptr, 0);
}

static inline void handleWifi(const uint8_t *p, int n) {
  if (n < 1 || p[0] + 1 > n) { sendError(E_INVALID); return; }
  String ssid((const char *)p + 1, p[0]);
  int o = 1 + p[0];
  String pass;
  if (o < n && o + 1 + p[o] <= n) pass = String((const char *)p + o + 1, p[o]);

  sendError(E_NONE);
  sendState(S_PROVISIONING);
  wifi_mode_t mode = WiFi.getMode();
  if (mode != WIFI_AP_STA) WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);

  if (WiFi.status() == WL_CONNECTED) {
    wifi_shared_save(ssid, pass);
    sendState(S_PROVISIONED);
    sendUrlResult(C_WIFI);
    delay(800);
    ESP.restart();  // l'appli redémarre proprement sur le nouveau réseau
  }
  sendError(E_CONNECT);
  sendState(S_READY);
  // retour sur le réseau enregistré, s'il y en a un
  String s, pw;
  if (wifi_shared_load(s, pw)) WiFi.begin(s.c_str(), pw.c_str());
}

static inline void handleScan() {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i), rssi = String(WiFi.RSSI(i));
    if (!ssid.length()) continue;
    bool dup = false;  // un seul résultat par nom de réseau
    for (int j = 0; j < i && !dup; j++) dup = WiFi.SSID(j) == ssid;
    if (dup) continue;
    const char *s[3] = {ssid.c_str(), rssi.c_str(), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
    sendResult(C_SCAN, s, 3);
  }
  WiFi.scanDelete();
  sendResult(C_SCAN, nullptr, 0);
}

static inline void handlePacket(uint8_t type, const uint8_t *d, int n) {
  if (type != T_RPC || n < 2 || d[1] + 2 > n) { sendError(E_INVALID); return; }
  uint8_t cmd = d[0];
  const uint8_t *p = d + 2;
  int len = d[1];
  switch (cmd) {
    case C_WIFI: handleWifi(p, len); break;
    case C_STATE: {
      uint8_t s = currentState();
      sendState(s);
      if (s == S_PROVISIONED) sendUrlResult(C_STATE);
      break;
    }
    case C_INFO: {
      const char *s[4] = {fwName, fwVersion, "ESP32-S3", fwName};
      sendResult(C_INFO, s, 4);
      break;
    }
    case C_SCAN: handleScan(); break;
    default: sendError(E_UNKNOWN_CMD);
  }
}
}  // namespace improv_detail

// Transmet les lignes texte reçues sur le port série (ce qui n'est pas un paquet Improv).
static inline void improv_set_line_hook(void (*hook)(const char *)) { improv_detail::lineHook = hook; }

static inline void improv_begin(const char *name, const char *version, const char *url) {
  improv_detail::fwName = name;
  improv_detail::fwVersion = version;
  improv_detail::url = url ? url : "";
}

// Lit les octets reçus sur le port série USB et répond aux paquets Improv.
static inline void improv_poll() {
  using namespace improv_detail;
  static const char HDR[] = "IMPROV";
  while (Serial.available()) {
    uint8_t c = Serial.read();
    if (pos < 6) {
      if (c == (uint8_t)HDR[pos]) { buf[pos++] = c; continue; }
      for (int i = 0; i < pos; i++) textByte(buf[i]);  // début d'en-tête qui n'en était pas un
      pos = 0;
      if (c == 'I') buf[pos++] = c;
      else textByte(c);
      continue;
    }
    buf[pos++] = c;
    if (pos == 7 && buf[6] != 1) { pos = 0; continue; }  // version 1 uniquement
    if (pos >= 9) {
      int len = buf[8];
      if (pos == 9 + len + 1) {
        uint8_t sum = 0;
        for (int i = 0; i < 9 + len; i++) sum += buf[i];
        uint8_t type = buf[7];
        pos = 0;
        if (sum == buf[9 + len]) handlePacket(type, buf + 9, len);
        else sendError(E_INVALID);
      }
    }
    if (pos >= (int)sizeof(buf)) pos = 0;
  }
}

// delay() qui continue de répondre à Improv
static inline void improv_delay(uint32_t ms) {
  uint32_t t0 = millis();
  do { improv_poll(); delay(10); } while (millis() - t0 < ms);
}
