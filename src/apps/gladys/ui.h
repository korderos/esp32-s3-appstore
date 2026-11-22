#pragma once
#include <stdint.h>

// Dernières valeurs lues dans Gladys (puissances en kW, énergies en kWh).
struct EnergyData {
  bool hasProd, hasCons, hasGrid;
  float prodKw, consKw, gridKw;      // gridKw > 0 : achat au réseau, < 0 : injection
  bool hasConsToday;
  float consTodayKwh;
  bool hasDaily;                     // compteurs du jour calculés à partir des index
  float prodTodayKwh, importTodayKwh, exportTodayKwh;
  char dailySince[6];                // "" = depuis minuit, sinon "HH:MM"
  bool hasExt, hasInt;
  float tExt, tInt;
  char ageExt[16], ageInt[16];       // ancienneté de la mesure, ex. "30 min"
};

extern volatile bool g_reqRefresh;    // bouton "Actualiser"
extern volatile bool g_reqWifiReset;  // appui long sur "Wi-Fi" dans les réglages

// Toutes ces fonctions doivent être appelées avec le verrou LVGL (lvgl_lock) pris.
void ui_init();
void ui_set_data(const EnergyData &d);
void ui_add_sample(float prodKw, float consKw);  // une fois par minute, pour la courbe
void ui_set_status(const char *txt, bool ok);
void ui_set_info(const char *txt);
void ui_show_message(const char *title, const char *msg);
void ui_show_main();
void ui_set_battery(int pct, bool usb);
