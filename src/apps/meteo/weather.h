#pragma once
#include <stdint.h>

// Données météo d'une ville (API Open-Meteo), partagées entre la boucle réseau et l'UI.
#define MAX_CITIES 8
#define WX_HOURS   24
#define WX_DAYS    7

struct WxDay {
  uint8_t code;        // code météo WMO
  int8_t wday;         // 0 = dimanche
  float tmax, tmin;    // °C
  float rain;          // cumul de précipitations (mm)
  float uv;            // indice UV max
  uint8_t pop;         // probabilité de précipitation max (%)
  char rise[6], set[6];  // "HH:MM" heure locale
};

struct WxData {
  // conditions actuelles
  float temp, feels, wind, gust, pressure, precip;
  uint8_t hum, cloud, code;
  int16_t windDir;
  bool isDay;
  int32_t utcOffset;   // décalage de l'heure locale de la ville (s)
  // 24 prochaines heures
  int8_t startHour;
  int nh;
  float hTemp[WX_HOURS];
  uint8_t hPop[WX_HOURS];
  // 7 jours
  int nd;
  WxDay d[WX_DAYS];
};
