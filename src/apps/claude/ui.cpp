// Interface LVGL 8 — écran paysage 640 x 172
//
//  0                        228                 438                       640
//  | Depenses du mois       | 30 derniers jours | Par modele    bat  ⚙   |
//  |   123,45 $             |  ▂▃▅▂▇▅▃▂ ...     | Opus 5.5        80,12 $ |
//  | Hier / Aujourd'hui     |                   | ▬▬▬▬▬▬▬                 |
//  | tokens · requetes      |                   | Sonnet 5.5      30,02 $ |
//  | budget [=====]  proj.  | 24/10   moy  22/11| ...                     |
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include "ui.h"

volatile bool g_reqRefresh = false;
volatile bool g_reqWifiReset = false;

#define C_BG      lv_color_hex(0x0B0E14)
#define C_PANEL   lv_color_hex(0x151A23)
#define C_TEXT    lv_color_hex(0xF2F4F8)
#define C_MUTED   lv_color_hex(0x7D8696)
#define C_GRID    lv_color_hex(0x1E2430)
#define C_CLAY    lv_color_hex(0xD97757)   // couleur d'accent
#define C_OK      lv_color_hex(0x16C784)
#define C_WARN    lv_color_hex(0xFFC53D)
#define C_BAD     lv_color_hex(0xEA3943)

static lv_obj_t *scr_main, *scr_msg, *settings, *info_lbl, *msg_title, *msg_body;
static lv_obj_t *month_lbl, *days_lbl, *tokens_lbl, *budget_lbl, *budget_track, *budget_bar, *status_lbl, *bat_lbl;
static lv_obj_t *chart, *chart_max, *chart_first, *chart_last, *chart_avg, *chart_info;
static lv_chart_series_t *ser;
static lv_obj_t *m_name[MAX_MODELS + 1], *m_val[MAX_MODELS + 1], *m_bar[MAX_MODELS + 1];
static char s_status[48] = "Demarrage";
static bool s_statusOk = true;
static char s_refreshed[16] = "";

// ---------- formatage : "1 234,56 $" ----------
static void fmt_usd(char *out, size_t n, float v) {
  int dec = v >= 1000 ? 0 : 2;
  char raw[24];
  snprintf(raw, sizeof(raw), "%.*f", dec, fabsf(v));
  char *dot = strchr(raw, '.');
  int intLen = dot ? (int)(dot - raw) : (int)strlen(raw);
  char buf[32];
  int o = 0;
  for (int i = 0; i < intLen; i++) {
    if (i > 0 && (intLen - i) % 3 == 0) buf[o++] = ' ';
    buf[o++] = raw[i];
  }
  if (dot) { buf[o++] = ','; strcpy(buf + o, dot + 1); o += strlen(dot + 1); }
  buf[o] = 0;
  snprintf(out, n, "%s $", buf);
}

static void fmt_big(char *out, size_t n, double v) {
  if (v >= 1e9) snprintf(out, n, "%.1f G", v / 1e9);
  else if (v >= 1e6) snprintf(out, n, "%.1f M", v / 1e6);
  else if (v >= 1e3) snprintf(out, n, "%.1f k", v / 1e3);
  else snprintf(out, n, "%.0f", v);
  for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

// couleur par famille de modèle
static lv_color_t model_color(const char *name) {
  if (strstr(name, "Opus")) return lv_color_hex(0xB26BFF);
  if (strstr(name, "Sonnet")) return C_CLAY;
  if (strstr(name, "Haiku")) return C_OK;
  if (strstr(name, "Fable") || strstr(name, "Mythos")) return lv_color_hex(0x4EA8FF);
  return C_MUTED;
}

// ---------- helpers ----------
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t *mk_button(lv_obj_t *parent, int w, int h, const char *txt) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, C_PANEL, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3240), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  if (txt) {
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_14, C_TEXT, txt);
    lv_obj_center(l);
  }
  return b;
}

static void mk_sep(lv_obj_t *parent, int x) {
  lv_obj_t *s = mk_box(parent, x, 10, 1, 152);
  lv_obj_set_style_bg_color(s, C_GRID, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
}

static void refresh_status() {
  if (!s_statusOk) {
    lv_label_set_text_fmt(status_lbl, LV_SYMBOL_WARNING " %s", s_status);
    lv_obj_set_style_text_color(status_lbl, C_BAD, 0);
  } else if (s_refreshed[0]) {
    lv_label_set_text_fmt(status_lbl, "Donnees jusqu'au %s", s_refreshed);
    lv_obj_set_style_text_color(status_lbl, C_MUTED, 0);
  } else {
    lv_label_set_text(status_lbl, s_status);
    lv_obj_set_style_text_color(status_lbl, C_MUTED, 0);
  }
}

// ---------- événements ----------
static void open_settings_cb(lv_event_t *) {
  lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(settings);
}
static void close_settings_cb(lv_event_t *) { lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); }
static void refresh_cb(lv_event_t *) { g_reqRefresh = true; lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); }
static void wifi_reset_cb(lv_event_t *) { g_reqWifiReset = true; }

// ---------- construction ----------
static void build_col_a() {
  lv_obj_t *t = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Depenses du mois");
  lv_obj_set_pos(t, 12, 6);
  month_lbl = mk_label(scr_main, &lv_font_montserrat_40, C_TEXT, "--");
  lv_obj_set_pos(month_lbl, 10, 20);
  days_lbl = mk_label(scr_main, &lv_font_montserrat_14, C_TEXT, "");
  lv_label_set_recolor(days_lbl, true);
  lv_obj_set_pos(days_lbl, 12, 66);
  tokens_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(tokens_lbl, 12, 88);
  budget_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_label_set_recolor(budget_lbl, true);
  lv_obj_set_width(budget_lbl, 210);
  lv_obj_set_pos(budget_lbl, 12, 108);
  budget_track = mk_box(scr_main, 12, 128, 204, 8);
  lv_obj_set_style_radius(budget_track, 4, 0);
  lv_obj_set_style_bg_color(budget_track, C_GRID, 0);
  lv_obj_set_style_bg_opa(budget_track, LV_OPA_COVER, 0);
  budget_bar = mk_box(budget_track, 0, 0, 0, 8);
  lv_obj_set_style_radius(budget_bar, 4, 0);
  lv_obj_set_style_bg_opa(budget_bar, LV_OPA_COVER, 0);
  lv_obj_add_flag(budget_track, LV_OBJ_FLAG_HIDDEN);
  status_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(status_lbl, 212);
  lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(status_lbl, 12, 150);
}

static void build_col_b() {
  const int X = 238, W = 190;
  lv_obj_t *t = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "30 derniers jours");
  lv_obj_set_pos(t, X, 6);
  chart_max = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(chart_max, 70);
  lv_obj_set_style_text_align(chart_max, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(chart_max, X + W - 70, 6);

  chart = lv_chart_create(scr_main);
  lv_obj_set_size(chart, W, 104);
  lv_obj_set_pos(chart, X, 26);
  lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_chart_set_point_count(chart, 2);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_pad_column(chart, 1, 0);
  lv_obj_set_style_line_color(chart, C_GRID, LV_PART_MAIN);
  lv_obj_set_style_radius(chart, 2, LV_PART_ITEMS);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
  ser = lv_chart_add_series(chart, C_CLAY, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);  // LVGL divise par (points - 1) : rien tant qu'il n'y a pas de données
  chart_info = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Chargement...");
  lv_obj_set_width(chart_info, W);
  lv_obj_set_style_text_align(chart_info, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(chart_info, X, 70);

  chart_first = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(chart_first, X, 134);
  chart_last = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(chart_last, 50);
  lv_obj_set_style_text_align(chart_last, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(chart_last, X + W - 50, 134);
  chart_avg = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(chart_avg, W);
  lv_obj_set_style_text_align(chart_avg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(chart_avg, X, 152);
}

static void build_col_c() {
  const int X = 446;
  lv_obj_t *t = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Par modele");
  lv_obj_set_pos(t, X, 8);
  bat_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(bat_lbl, 60);
  lv_obj_set_style_text_align(bat_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(bat_lbl, 540, 8);
  lv_obj_t *gear = lv_btn_create(scr_main);
  lv_obj_remove_style_all(gear);
  lv_obj_set_size(gear, 32, 28);
  lv_obj_set_pos(gear, 604, 2);
  lv_obj_set_style_radius(gear, 8, 0);
  lv_obj_set_style_bg_color(gear, C_PANEL, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *gl = mk_label(gear, &lv_font_montserrat_16, C_MUTED, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);
  lv_obj_add_event_cb(gear, open_settings_cb, LV_EVENT_CLICKED, NULL);

  for (int i = 0; i <= MAX_MODELS; i++) {
    int y = 32 + i * 23;
    m_name[i] = mk_label(scr_main, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_pos(m_name[i], X, y);
    m_val[i] = mk_label(scr_main, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(m_val[i], 90);
    lv_obj_set_style_text_align(m_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(m_val[i], 634 - 90, y);
    m_bar[i] = mk_box(scr_main, X, y + 18, 0, 3);
    lv_obj_set_style_radius(m_bar[i], 1, 0);
    lv_obj_set_style_bg_opa(m_bar[i], LV_OPA_COVER, 0);
  }
}

static void build_settings() {
  settings = lv_obj_create(scr_main);
  lv_obj_remove_style_all(settings);
  lv_obj_set_size(settings, 640, 172);
  lv_obj_set_style_bg_color(settings, C_BG, 0);
  lv_obj_set_style_bg_opa(settings, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settings, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t = mk_label(settings, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_SETTINGS "  Reglages");
  lv_obj_set_pos(t, 16, 12);
  info_lbl = mk_label(settings, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_set_pos(info_lbl, 16, 46);
  lv_obj_set_width(info_lbl, 420);
  struct { const char *txt; lv_event_cb_t cb; lv_event_code_t ev; } btns[3] = {
    {LV_SYMBOL_REFRESH "  Actualiser", refresh_cb, LV_EVENT_CLICKED},
    {LV_SYMBOL_WIFI "  Wi-Fi (appui long)", wifi_reset_cb, LV_EVENT_LONG_PRESSED},
    {LV_SYMBOL_CLOSE "  Fermer", close_settings_cb, LV_EVENT_CLICKED},
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *b = mk_button(settings, 176, 44, btns[i].txt);
    lv_obj_set_pos(b, 448, 10 + i * 52);
    lv_obj_add_event_cb(b, btns[i].cb, btns[i].ev, NULL);
  }
  lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN);
}

void ui_init() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, C_BG, 0);
  lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
  build_col_a();
  mk_sep(scr_main, 228);
  build_col_b();
  mk_sep(scr_main, 438);
  build_col_c();
  build_settings();

  scr_msg = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_msg, C_BG, 0);
  lv_obj_clear_flag(scr_msg, LV_OBJ_FLAG_SCROLLABLE);
  msg_title = mk_label(scr_msg, &lv_font_montserrat_28, C_TEXT, "");
  lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 18);
  msg_body = mk_label(scr_msg, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_set_width(msg_body, 600);
  lv_obj_set_style_text_align(msg_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 64);
  refresh_status();
  lv_scr_load(scr_msg);
}

// ---------- API publique ----------
void ui_set_data(const ClaudeData &d) {
  char a[32], b[32];
  fmt_usd(a, sizeof(a), d.monthUsd);
  lv_label_set_text(month_lbl, a);

  // hier / aujourd'hui (aujourd'hui est partiel : les données arrivent avec 4 à 24 h de retard)
  if (d.nDays >= 2) {
    fmt_usd(a, sizeof(a), d.dayUsd[d.nDays - 2]);
    fmt_usd(b, sizeof(b), d.dayUsd[d.nDays - 1]);
    lv_label_set_text_fmt(days_lbl, "#7D8696 Hier# %s   #7D8696 Auj.# %s", a, b);
  } else {
    lv_label_set_text(days_lbl, "");
  }
  if (d.hasUsage) {
    fmt_big(a, sizeof(a), d.tokens);
    fmt_big(b, sizeof(b), d.requests);
    lv_label_set_text_fmt(tokens_lbl, "%s tokens  -  %s requetes", a, b);
  } else {
    lv_label_set_text(tokens_lbl, "");
  }

  // budget et projection
  fmt_usd(a, sizeof(a), d.projectionUsd);
  if (d.budgetUsd > 0) {
    float pct = d.monthUsd / d.budgetUsd;
    bool over = d.projectionUsd > d.budgetUsd;
    snprintf(b, sizeof(b), "%d $", (int)lroundf(d.budgetUsd));
    lv_label_set_text_fmt(budget_lbl, "Fin de mois #%s ~%s# / budget %s", over ? "EA3943" : "16C784", a, b);
    lv_obj_clear_flag(budget_track, LV_OBJ_FLAG_HIDDEN);
    int w = (int)lroundf(fminf(pct, 1.0f) * 204);
    lv_obj_set_width(budget_bar, LV_MAX(w, pct > 0 ? 4 : 0));
    lv_obj_set_style_bg_color(budget_bar, pct >= 1 ? C_BAD : pct >= 0.8f ? C_WARN : C_OK, 0);
  } else {
    lv_label_set_text_fmt(budget_lbl, "Estimation fin de mois : %s", a);
    lv_obj_add_flag(budget_track, LV_OBJ_FLAG_HIDDEN);
  }

  // histogramme des 30 derniers jours
  float mx = 0, sum = 0;
  for (int i = 0; i < d.nDays; i++) { mx = fmaxf(mx, d.dayUsd[i]); sum += d.dayUsd[i]; }
  if (d.nDays >= 2 && mx >= 0.005f) {
    lv_chart_set_point_count(chart, d.nDays);
    for (int i = 0; i < d.nDays; i++) ser->y_points[i] = (lv_coord_t)lroundf(d.dayUsd[i] / mx * 1000);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1050);
    lv_chart_refresh(chart);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
    fmt_usd(a, sizeof(a), mx);
    lv_label_set_text_fmt(chart_max, "max %s", a);
    lv_label_set_text(chart_first, d.firstDay);
    lv_label_set_text(chart_last, d.lastDay);
    fmt_usd(a, sizeof(a), sum / d.nDays);
    lv_label_set_text_fmt(chart_avg, "moyenne %s / jour", a);
  } else {
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(chart_info, d.nDays >= 2 ? "Aucune depense\nsur 30 jours" : "Pas encore de donnees");
    lv_label_set_text(chart_max, "");
    lv_label_set_text(chart_first, d.firstDay);
    lv_label_set_text(chart_last, d.lastDay);
    lv_label_set_text(chart_avg, "");
  }

  // par modèle
  float top = d.nModels ? d.modelUsd[0] : 0;
  for (int i = 0; i <= MAX_MODELS; i++) {
    bool other = i == d.nModels && d.otherUsd > 0.005f;
    if (i < d.nModels || other) {
      const char *name = other ? "Autres" : d.model[i];
      float v = other ? d.otherUsd : d.modelUsd[i];
      lv_label_set_text(m_name[i], name);
      lv_obj_set_style_text_color(m_name[i], other ? C_MUTED : C_TEXT, 0);
      fmt_usd(a, sizeof(a), v);
      lv_label_set_text(m_val[i], a);
      lv_obj_set_width(m_bar[i], top > 0 ? LV_MAX(2, (int)lroundf(v / top * 188)) : 0);
      lv_obj_set_style_bg_color(m_bar[i], model_color(name), 0);
      lv_obj_clear_flag(m_bar[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(m_name[i], i == 0 ? "Aucune depense\nce mois-ci" : "");
      lv_obj_set_style_text_color(m_name[i], i == 0 ? C_MUTED : C_TEXT, 0);
      lv_label_set_text(m_val[i], "");
      lv_obj_add_flag(m_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  snprintf(s_refreshed, sizeof(s_refreshed), "%s", d.refreshed);
  s_statusOk = true;
  refresh_status();
}

void ui_set_status(const char *txt, bool ok) {
  snprintf(s_status, sizeof(s_status), "%s", txt);
  s_statusOk = ok;
  refresh_status();
}

void ui_set_info(const char *txt) { lv_label_set_text(info_lbl, txt); }

void ui_show_message(const char *title, const char *msg) {
  lv_label_set_text(msg_title, title);
  lv_label_set_text(msg_body, msg);
  if (lv_scr_act() != scr_msg) lv_scr_load(scr_msg);
}

void ui_show_main() {
  if (lv_scr_act() != scr_main) lv_scr_load(scr_main);
}

void ui_set_battery(int pct, bool usb) {
  const char *icon = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
                   : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  if (usb) lv_label_set_text_fmt(bat_lbl, LV_SYMBOL_CHARGE " %d%%", pct);
  else lv_label_set_text_fmt(bat_lbl, "%s %d%%", icon, pct);
  lv_obj_set_style_text_color(bat_lbl, usb ? C_OK : (pct <= 15 ? C_BAD : C_MUTED), 0);
}
