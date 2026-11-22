#pragma once
#include <stdint.h>
#include "weather.h"

// État partagé avec la boucle réseau (écrit par l'UI, lu par loop()).
extern volatile int g_city;           // ville affichée
extern volatile int g_view;           // 0 = 24 h, 1 = 7 jours
extern volatile bool g_reqRefresh;    // bouton "Actualiser"
extern volatile bool g_reqWifiReset;  // appui long sur "Wi-Fi" dans les réglages

// Toutes ces fonctions doivent être appelées avec le verrou LVGL (lvgl_lock) pris.
void ui_init(int nCities, const char *const *names, int city, int view);
void ui_set_city(int i, const char *name, const char *region);  // texte ASCII
void ui_set_city_error(int i, const char *msg);
void ui_set_weather(int i, const WxData &d);
void ui_set_status(const char *txt, bool ok);
void ui_set_info(const char *txt);    // texte de l'écran Réglages
void ui_show_message(const char *title, const char *msg);  // écran plein (connexion, config)
void ui_show_main();
void ui_set_battery(int pct, bool usb);
