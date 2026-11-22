#pragma once
#include <stdint.h>

#define MAX_MODELS 5
#define MAX_DAYS   30

// Dépenses et consommation de l'organisation Claude (Admin API, Usage and Cost Analytics).
struct ClaudeData {
  float monthUsd;                  // mois en cours (UTC)
  int nModels;                     // modèles triés par coût décroissant
  char model[MAX_MODELS][24];
  float modelUsd[MAX_MODELS];
  float otherUsd;                  // modèles au-delà des MAX_MODELS premiers
  int nDays;                       // 30 derniers jours, le plus ancien d'abord ; le dernier = aujourd'hui (partiel)
  float dayUsd[MAX_DAYS];
  char firstDay[6], lastDay[6];    // "JJ/MM"
  bool hasUsage;
  double tokens;                   // tokens du mois (entrée + cache + sortie)
  uint32_t requests;
  float budgetUsd;                 // 0 = pas de budget
  float projectionUsd;             // estimation fin de mois
  char refreshed[16];              // "JJ/MM HH:MM" heure locale, fin des données complètes
};

extern volatile bool g_reqRefresh;
extern volatile bool g_reqWifiReset;

// Toutes ces fonctions doivent être appelées avec le verrou LVGL (lvgl_lock) pris.
void ui_init();
void ui_set_data(const ClaudeData &d);
void ui_set_status(const char *txt, bool ok);
void ui_set_info(const char *txt);
void ui_show_message(const char *title, const char *msg);
void ui_show_main();
void ui_set_battery(int pct, bool usb);
