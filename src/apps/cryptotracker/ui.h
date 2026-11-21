#pragma once
#include <stdint.h>

// État partagé avec la boucle réseau (écrit par l'UI, lu par loop()).
extern volatile int g_coin;           // crypto affichée
extern volatile int g_tf;             // période du graphique
extern volatile bool g_reqChart;      // l'UI demande un rechargement immédiat du graphique
extern volatile bool g_reqWifiReset;  // appui long sur "Wi-Fi" dans le sélecteur

// Toutes ces fonctions doivent être appelées avec le verrou LVGL (lvgl_lock) pris.
void ui_init(int coin, int tf);
void ui_set_ticker(int coin, double price, double chgPct, double high, double low);
void ui_set_chart(int coin, int tf, const double *closes, int n);
void ui_set_status(const char *txt, bool ok);
void ui_show_message(const char *title, const char *msg);  // écran plein (connexion, config)
void ui_show_main();
void ui_set_battery(int pct, bool usb);  // usb = alimenté/en charge par l'USB
