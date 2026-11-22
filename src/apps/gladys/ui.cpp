// Interface LVGL 8 — écran paysage 640 x 172
//
//  0                         304            446                    640
//  | (o) Production   auj.  | Exterieur    | 3 dernieres h   bat ⚙ |
//  | (o) Consommation auj.  |   10,6°      |  courbe prod / conso  |
//  | (o) Reseau       auj.  | Interieur    |                       |
//  | [== couverture solaire]|   20,0°      | MAJ ...               |
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
#define C_ACCENT  lv_color_hex(0x3B82F6)
#define C_SUN     lv_color_hex(0xFFC53D)
#define C_HOME    lv_color_hex(0x4EA8FF)
#define C_BUY     lv_color_hex(0xEA3943)
#define C_SELL    lv_color_hex(0x16C784)
#define C_WARM    lv_color_hex(0xFF9F43)

#define HIST 180  // 3 h à raison d'un point par minute

static lv_obj_t *scr_main, *scr_msg, *settings, *info_lbl, *msg_title, *msg_body;
static lv_obj_t *row_val[3], *row_lbl[3], *row_day[3], *row_badge[3];
static lv_obj_t *cover_bar, *cover_lbl;
static lv_obj_t *ext_val, *ext_age, *int_val, *int_age;
static lv_obj_t *chart, *chart_info, *max_lbl, *status_lbl, *bat_lbl;
static lv_chart_series_t *ser_prod, *ser_cons;
static float s_prod[HIST], s_cons[HIST];
static int s_n = 0;
static uint32_t s_lastOkMs = 0;
static char s_status[48] = "Demarrage";
static bool s_statusOk = true;

// ---------- formatage (virgule décimale) ----------
static void fmt(char *out, size_t n, float v, int dec, const char *unit) {
  snprintf(out, n, "%.*f%s", dec, v, unit);
  for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

static void fmt_kw(char *out, size_t n, float kw) {
  float a = fabsf(kw);
  if (a < 1.0f) snprintf(out, n, "%d W", (int)lroundf(a * 1000));
  else fmt(out, n, a, a < 10 ? 2 : 1, " kW");
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

// remplissage dégradé sous la courbe de production
static void chart_fill_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || !dsc->p1 || !dsc->p2 || dsc->sub_part_ptr != ser_prod) return;
  lv_draw_mask_line_param_t lm;
  lv_draw_mask_line_points_init(&lm, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y, LV_DRAW_MASK_LINE_SIDE_BOTTOM);
  int16_t lid = lv_draw_mask_add(&lm, NULL);
  lv_draw_mask_fade_param_t fm;
  lv_draw_mask_fade_init(&fm, &obj->coords, LV_OPA_COVER, obj->coords.y1, LV_OPA_TRANSP, obj->coords.y2);
  int16_t fid = lv_draw_mask_add(&fm, NULL);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = LV_OPA_40;
  rd.bg_color = C_SUN;
  lv_area_t a;
  a.x1 = dsc->p1->x;
  a.x2 = dsc->p2->x - 1;
  a.y1 = LV_MIN(dsc->p1->y, dsc->p2->y);
  a.y2 = obj->coords.y2;
  lv_draw_rect(dsc->draw_ctx, &rd, &a);
  lv_draw_mask_free_param(&lm);
  lv_draw_mask_free_param(&fm);
  lv_draw_mask_remove_id(lid);
  lv_draw_mask_remove_id(fid);
}

// ---------- rafraîchissements ----------
static void refresh_chart() {
  if (s_n < 2) {
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);  // LVGL divise par (points - 1)
    lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(max_lbl, "");
    return;
  }
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
  float mx = 0.5f;
  for (int i = 0; i < s_n; i++) mx = fmaxf(mx, fmaxf(s_prod[i], s_cons[i]));
  lv_chart_set_point_count(chart, s_n);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)lroundf(mx * 110));  // 10 % de marge
  for (int i = 0; i < s_n; i++) {
    ser_prod->y_points[i] = (lv_coord_t)lroundf(s_prod[i] * 100);
    ser_cons->y_points[i] = (lv_coord_t)lroundf(s_cons[i] * 100);
  }
  lv_chart_refresh(chart);
  char buf[16];
  fmt_kw(buf, sizeof(buf), mx);
  lv_label_set_text(max_lbl, buf);
}

static void refresh_status() {
  if (!s_statusOk || !s_lastOkMs) {
    lv_label_set_text_fmt(status_lbl, "%s%s", s_statusOk ? "" : LV_SYMBOL_WARNING " ", s_status);
    lv_obj_set_style_text_color(status_lbl, s_statusOk ? C_MUTED : C_BUY, 0);
    return;
  }
  uint32_t s = (millis() - s_lastOkMs) / 1000;
  if (s < 60) lv_label_set_text_fmt(status_lbl, LV_SYMBOL_REFRESH " il y a %u s", (unsigned)s);
  else lv_label_set_text_fmt(status_lbl, LV_SYMBOL_REFRESH " il y a %u min", (unsigned)(s / 60));
  lv_obj_set_style_text_color(status_lbl, s >= 300 ? C_WARM : C_MUTED, 0);
}

static void tick_cb(lv_timer_t *) { refresh_status(); }

// ---------- événements ----------
static void open_settings_cb(lv_event_t *) {
  lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(settings);
}
static void close_settings_cb(lv_event_t *) { lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); }
static void refresh_cb(lv_event_t *) { g_reqRefresh = true; lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); }
static void wifi_reset_cb(lv_event_t *) { g_reqWifiReset = true; }

// ---------- construction ----------
static const char *ROW_NAMES[3] = {"Production solaire", "Consommation maison", "Reseau"};
static const char *ROW_SYMS[3] = {LV_SYMBOL_CHARGE, LV_SYMBOL_HOME, LV_SYMBOL_POWER};

static void build_col_a() {
  const lv_color_t cols[3] = {C_SUN, C_HOME, C_MUTED};
  for (int i = 0; i < 3; i++) {
    int y = 4 + i * 47;
    row_badge[i] = mk_box(scr_main, 10, y + 6, 34, 34);
    lv_obj_set_style_radius(row_badge[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row_badge[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row_badge[i], cols[i], 0);
    lv_obj_t *s = mk_label(row_badge[i], &lv_font_montserrat_16, C_BG, ROW_SYMS[i]);
    lv_obj_center(s);
    row_lbl[i] = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, ROW_NAMES[i]);
    lv_obj_set_pos(row_lbl[i], 54, y);
    row_val[i] = mk_label(scr_main, &lv_font_montserrat_28, C_TEXT, "--");
    lv_obj_set_pos(row_val[i], 54, y + 14);
    row_day[i] = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_recolor(row_day[i], true);
    lv_obj_set_width(row_day[i], 110);
    lv_obj_set_style_text_align(row_day[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(row_day[i], 188, y + 6);
  }
  // part de la consommation couverte par le solaire (maintenant)
  lv_obj_t *track = mk_box(scr_main, 10, 152, 150, 8);
  lv_obj_set_style_radius(track, 4, 0);
  lv_obj_set_style_bg_color(track, C_GRID, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  cover_bar = mk_box(track, 0, 0, 0, 8);
  lv_obj_set_style_radius(cover_bar, 4, 0);
  lv_obj_set_style_bg_color(cover_bar, C_SUN, 0);
  lv_obj_set_style_bg_opa(cover_bar, LV_OPA_COVER, 0);
  cover_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(cover_lbl, 168, 148);
}

static void build_col_b() {
  lv_obj_t *l = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Exterieur");
  lv_obj_set_pos(l, 316, 6);
  ext_val = mk_label(scr_main, &lv_font_montserrat_40, C_TEXT, "--");
  lv_obj_set_pos(ext_val, 314, 20);
  ext_age = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(ext_age, 316, 64);
  l = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Interieur");
  lv_obj_set_pos(l, 316, 92);
  int_val = mk_label(scr_main, &lv_font_montserrat_28, C_TEXT, "--");
  lv_obj_set_pos(int_val, 316, 106);
  int_age = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(int_age, 316, 140);
}

static void build_col_c() {
  const int X = 456, W = 178;
  lv_obj_t *t = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Sur 3 heures");
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

  chart = lv_chart_create(scr_main);
  lv_obj_set_size(chart, W, 96);
  lv_obj_set_pos(chart, X, 30);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_chart_set_point_count(chart, 2);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_line_color(chart, C_GRID, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
  ser_cons = lv_chart_add_series(chart, C_HOME, LV_CHART_AXIS_PRIMARY_Y);
  ser_prod = lv_chart_add_series(chart, C_SUN, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(chart, chart_fill_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

  max_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(max_lbl, X + 2, 30);
  chart_info = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "La courbe se remplit\nune fois par minute");
  lv_obj_set_style_text_align(chart_info, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(chart_info, W);
  lv_obj_set_pos(chart_info, X, 60);

  lv_obj_t *legend = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "#FFC53D Production#    #4EA8FF Conso#");
  lv_label_set_recolor(legend, true);
  lv_obj_set_pos(legend, X, 130);
  status_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(status_lbl, W);
  lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(status_lbl, X, 150);
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
  mk_sep(scr_main, 304);
  build_col_b();
  mk_sep(scr_main, 446);
  build_col_c();
  build_settings();
  lv_timer_create(tick_cb, 5000, NULL);

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
void ui_set_data(const EnergyData &d) {
  char buf[32], kwh[16];
  // production
  if (d.hasProd) { fmt_kw(buf, sizeof(buf), d.prodKw); lv_label_set_text(row_val[0], buf); }
  else lv_label_set_text(row_val[0], "--");
  // consommation
  if (d.hasCons) { fmt_kw(buf, sizeof(buf), d.consKw); lv_label_set_text(row_val[1], buf); }
  else lv_label_set_text(row_val[1], "--");
  // réseau : achat (rouge) ou injection (vert)
  if (d.hasGrid) {
    bool sell = d.gridKw < -0.005f;
    fmt_kw(buf, sizeof(buf), d.gridKw);
    lv_label_set_text(row_val[2], buf);
    lv_obj_set_style_text_color(row_val[2], sell ? C_SELL : C_TEXT, 0);
    lv_label_set_text(row_lbl[2], sell ? "Reseau - injection" : "Reseau - achat");
    lv_obj_set_style_bg_color(row_badge[2], sell ? C_SELL : C_BUY, 0);
  } else {
    lv_label_set_text(row_val[2], "--");
    lv_obj_set_style_bg_color(row_badge[2], C_MUTED, 0);
  }

  // énergies du jour
  const char *since = d.dailySince[0] ? d.dailySince : NULL;
  if (d.hasDaily) {
    fmt(kwh, sizeof(kwh), d.prodTodayKwh, 1, " kWh");
    if (since) lv_label_set_text_fmt(row_day[0], "depuis %s\n#F2F4F8 %s#", since, kwh);
    else lv_label_set_text_fmt(row_day[0], "aujourd'hui\n#F2F4F8 %s#", kwh);
    char imp[16], exp[16];
    fmt(imp, sizeof(imp), d.importTodayKwh, 1, "");
    fmt(exp, sizeof(exp), d.exportTodayKwh, 1, "");
    lv_label_set_text_fmt(row_day[2], "achat / injection\n#F2F4F8 %s# / #16C784 %s# kWh", imp, exp);
  } else {
    lv_label_set_text(row_day[0], "");
    lv_label_set_text(row_day[2], "");
  }
  if (d.hasConsToday) {
    fmt(kwh, sizeof(kwh), d.consTodayKwh, 1, " kWh");
    lv_label_set_text_fmt(row_day[1], "aujourd'hui\n#F2F4F8 %s#", kwh);
  } else {
    lv_label_set_text(row_day[1], "");
  }

  // couverture solaire instantanée
  if (d.hasProd && d.hasCons && d.consKw > 0.01f) {
    int pct = (int)lroundf(fminf(d.prodKw / d.consKw, 1.0f) * 100);
    if (pct < 0) pct = 0;
    lv_obj_set_width(cover_bar, LV_MAX(pct * 150 / 100, pct ? 4 : 0));
    lv_label_set_text_fmt(cover_lbl, "%d %% solaire", pct);
  } else {
    lv_obj_set_width(cover_bar, 0);
    lv_label_set_text(cover_lbl, "");
  }

  // températures
  if (d.hasExt) {
    fmt(buf, sizeof(buf), d.tExt, 1, "\xC2\xB0");
    lv_label_set_text(ext_val, buf);
    lv_obj_set_style_text_color(ext_val, d.tExt < 0 ? C_HOME : d.tExt >= 30 ? C_WARM : C_TEXT, 0);
    lv_label_set_text_fmt(ext_age, "il y a %s", d.ageExt);
  } else {
    lv_label_set_text(ext_val, "--");
    lv_label_set_text(ext_age, "");
  }
  if (d.hasInt) {
    fmt(buf, sizeof(buf), d.tInt, 1, "\xC2\xB0");
    lv_label_set_text(int_val, buf);
    lv_label_set_text_fmt(int_age, "il y a %s", d.ageInt);
  } else {
    lv_label_set_text(int_val, "--");
    lv_label_set_text(int_age, "");
  }
  s_lastOkMs = millis();
  refresh_status();
}

void ui_add_sample(float prodKw, float consKw) {
  if (s_n == HIST) {
    memmove(s_prod, s_prod + 1, (HIST - 1) * sizeof(float));
    memmove(s_cons, s_cons + 1, (HIST - 1) * sizeof(float));
    s_n--;
  }
  s_prod[s_n] = fmaxf(prodKw, 0);
  s_cons[s_n] = fmaxf(consKw, 0);
  s_n++;
  refresh_chart();
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
  lv_obj_set_style_text_color(bat_lbl, usb ? C_SELL : (pct <= 15 ? C_BUY : C_MUTED), 0);
}
