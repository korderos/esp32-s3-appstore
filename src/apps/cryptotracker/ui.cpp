// Interface LVGL 8 — écran paysage 640 x 172
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include "ui.h"
#include "coins.h"

volatile int g_coin = 0;
volatile int g_tf = 1;
volatile bool g_reqChart = true;
volatile bool g_reqWifiReset = false;

// ---------- palette ----------
#define C_BG      lv_color_hex(0x0B0E14)
#define C_PANEL   lv_color_hex(0x151A23)
#define C_TEXT    lv_color_hex(0xF2F4F8)
#define C_MUTED   lv_color_hex(0x7D8696)
#define C_GRID    lv_color_hex(0x1E2430)
#define C_UP      lv_color_hex(0x16C784)
#define C_DOWN    lv_color_hex(0xEA3943)
#define C_ACCENT  lv_color_hex(0x3B82F6)

struct TickerCache { bool valid; double price, chg, high, low; };
static TickerCache s_tick[NUM_COINS];

// ---------- objets ----------
static lv_obj_t *scr_main, *scr_msg, *picker;
static lv_obj_t *badge, *badge_lbl, *sym_lbl, *name_lbl, *idx_lbl, *bat_lbl;
static lv_obj_t *price_lbl, *chg_chip, *chg_lbl, *hl_lbl;
static lv_obj_t *status_lbl, *chart, *max_lbl, *min_lbl, *chart_info;
static lv_obj_t *tf_btn[NUM_TFS];
static lv_obj_t *pick_btn[NUM_COINS];
static lv_chart_series_t *series;
static lv_obj_t *msg_title, *msg_body;

// ---------- formatage ----------
// Format français : espaces pour les milliers, virgule décimale.
static void fmt_num(double v, char *out, size_t len) {
  int dec;
  double a = fabs(v);
  if (a >= 10000) dec = 0;
  else if (a >= 1000) dec = 1;
  else if (a >= 1) dec = 2;
  else if (a >= 0.01) dec = 4;
  else dec = 6;

  char raw[32];
  snprintf(raw, sizeof(raw), "%.*f", dec, a);
  char *dot = strchr(raw, '.');
  int intLen = dot ? (int)(dot - raw) : (int)strlen(raw);

  char buf[40];
  int o = 0;
  if (v < 0) buf[o++] = '-';
  for (int i = 0; i < intLen; i++) {
    if (i > 0 && (intLen - i) % 3 == 0) buf[o++] = ' ';
    buf[o++] = raw[i];
  }
  if (dot) {
    buf[o++] = ',';
    strcpy(buf + o, dot + 1);
    o += strlen(dot + 1);
  }
  buf[o] = 0;
  snprintf(out, len, "%s", buf);
}

static void fmt_price(double v, char *out, size_t len) {
  char n[40];
  fmt_num(v, n, sizeof(n));
  snprintf(out, len, "%s%s", n, CURRENCY_SUFFIX);
}

// ---------- helpers ----------
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

static void no_scroll(lv_obj_t *o) {
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void select_coin(int idx);
static void refresh_header();
static void refresh_ticker();

// Remplissage dégradé sous la courbe (adapté de l'exemple LVGL lv_example_chart_2)
static void chart_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || !dsc->p1 || !dsc->p2) return;

  lv_draw_mask_line_param_t line_mask;
  lv_draw_mask_line_points_init(&line_mask, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y,
                                LV_DRAW_MASK_LINE_SIDE_BOTTOM);
  int16_t line_id = lv_draw_mask_add(&line_mask, NULL);

  lv_coord_t h = lv_obj_get_height(obj);
  lv_draw_mask_fade_param_t fade_mask;
  lv_draw_mask_fade_init(&fade_mask, &obj->coords, LV_OPA_COVER, obj->coords.y1 + h / 8,
                         LV_OPA_TRANSP, obj->coords.y2);
  int16_t fade_id = lv_draw_mask_add(&fade_mask, NULL);

  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = LV_OPA_30;
  rd.bg_color = dsc->line_dsc->color;

  lv_area_t a;
  a.x1 = dsc->p1->x;
  a.x2 = dsc->p2->x - 1;
  a.y1 = LV_MIN(dsc->p1->y, dsc->p2->y);
  a.y2 = obj->coords.y2;
  lv_draw_rect(dsc->draw_ctx, &rd, &a);

  lv_draw_mask_free_param(&line_mask);
  lv_draw_mask_free_param(&fade_mask);
  lv_draw_mask_remove_id(line_id);
  lv_draw_mask_remove_id(fade_id);
}

// ---------- événements ----------
static void gesture_cb(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) select_coin((g_coin + 1) % NUM_COINS);
  else if (dir == LV_DIR_RIGHT) select_coin((g_coin + NUM_COINS - 1) % NUM_COINS);
  else return;
  lv_indev_wait_release(lv_indev_get_act());
}

static void header_click_cb(lv_event_t *e) {
  for (int i = 0; i < NUM_COINS; i++) {
    if (i == g_coin) lv_obj_add_state(pick_btn[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(pick_btn[i], LV_STATE_CHECKED);
  }
  lv_obj_clear_flag(picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(picker);
}

static void pick_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
  select_coin(idx);
}

static void close_picker_cb(lv_event_t *e) {
  lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_reset_cb(lv_event_t *e) {
  g_reqWifiReset = true;
}

static void tf_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx == g_tf) return;
  g_tf = idx;
  for (int i = 0; i < NUM_TFS; i++) {
    if (i == idx) lv_obj_add_state(tf_btn[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(tf_btn[i], LV_STATE_CHECKED);
  }
  lv_chart_set_point_count(chart, 0);
  lv_label_set_text(chart_info, "Chargement...");
  lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(max_lbl, "");
  lv_label_set_text(min_lbl, "");
  g_reqChart = true;
}

static void select_coin(int idx) {
  if (idx == g_coin) return;
  g_coin = idx;
  refresh_header();
  refresh_ticker();
  lv_chart_set_point_count(chart, 0);
  lv_label_set_text(chart_info, "Chargement...");
  lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(max_lbl, "");
  lv_label_set_text(min_lbl, "");
  g_reqChart = true;
}

// ---------- rafraîchissements ----------
static void refresh_header() {
  const Coin &c = COINS[g_coin];
  lv_obj_set_style_bg_color(badge, lv_color_hex(c.color), 0);
  char letter[2] = {c.sym[0], 0};
  lv_label_set_text(badge_lbl, letter);
  lv_label_set_text_fmt(sym_lbl, "%s " LV_SYMBOL_DOWN, c.sym);
  lv_label_set_text(name_lbl, c.name);
  lv_label_set_text_fmt(idx_lbl, "%d/%d", g_coin + 1, NUM_COINS);
}

static void refresh_ticker() {
  const TickerCache &t = s_tick[g_coin];
  if (!t.valid) {
    lv_label_set_text(price_lbl, "--");
    lv_label_set_text(chg_lbl, "-- %");
    lv_obj_set_style_bg_color(chg_chip, C_PANEL, 0);
    lv_label_set_text(hl_lbl, "");
    return;
  }
  char buf[48], hi[32], lo[32];
  fmt_price(t.price, buf, sizeof(buf));
  lv_label_set_text(price_lbl, buf);

  bool up = t.chg >= 0;
  snprintf(buf, sizeof(buf), "%s %+.2f %%", up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, t.chg);
  for (char *p = buf; *p; p++) if (*p == '.') *p = ',';
  lv_label_set_text(chg_lbl, buf);
  lv_obj_set_style_bg_color(chg_chip, up ? C_UP : C_DOWN, 0);

  fmt_num(t.high, hi, sizeof(hi));
  fmt_num(t.low, lo, sizeof(lo));
  lv_label_set_text_fmt(hl_lbl, "Haut %s   Bas %s", hi, lo);
}

// ---------- API publique ----------
void ui_set_ticker(int coin, double price, double chg, double high, double low) {
  if (coin < 0 || coin >= NUM_COINS) return;
  s_tick[coin] = {true, price, chg, high, low};
  if (coin == g_coin) refresh_ticker();
}

void ui_set_chart(int coin, int tf, const double *v, int n) {
  if (coin != g_coin || tf != g_tf) return;  // l'utilisateur a changé entre-temps
  if (n < 2) {
    lv_chart_set_point_count(chart, 0);
    lv_label_set_text(chart_info, "Pas de donnees");
    lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  double mn = v[0], mx = v[0];
  for (int i = 1; i < n; i++) { mn = fmin(mn, v[i]); mx = fmax(mx, v[i]); }
  double span = mx - mn;
  if (span <= 0) span = mx * 0.001 + 1e-9;

  // lv_chart travaille en entiers 16 bits : on normalise sur 0..1000
  lv_chart_set_point_count(chart, n);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -40, 1040);
  for (int i = 0; i < n; i++) {
    series->y_points[i] = (lv_coord_t)lround((v[i] - mn) / span * 1000.0);
  }
  bool up = v[n - 1] >= v[0];
  lv_chart_set_series_color(chart, series, up ? C_UP : C_DOWN);
  lv_chart_refresh(chart);

  char buf[32];
  fmt_num(mx, buf, sizeof(buf));
  lv_label_set_text(max_lbl, buf);
  fmt_num(mn, buf, sizeof(buf));
  lv_label_set_text(min_lbl, buf);

  double pct = (v[n - 1] - v[0]) / v[0] * 100.0;
  snprintf(buf, sizeof(buf), "%+.2f %%", pct);
  for (char *p = buf; *p; p++) if (*p == '.') *p = ',';
  lv_label_set_text(chart_info, buf);
  lv_obj_set_style_text_color(chart_info, up ? C_UP : C_DOWN, 0);
  lv_obj_clear_flag(chart_info, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_status(const char *txt, bool ok) {
  lv_label_set_text_fmt(status_lbl, "%s %s", ok ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING, txt);
  lv_obj_set_style_text_color(status_lbl, ok ? C_MUTED : C_DOWN, 0);
}

void ui_show_message(const char *title, const char *msg) {
  lv_label_set_text(msg_title, title);
  lv_label_set_text(msg_body, msg);
  if (lv_scr_act() != scr_msg) lv_scr_load(scr_msg);
}

void ui_set_battery(int pct, bool usb) {
  const char *icon = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
                   : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  if (usb) lv_label_set_text_fmt(bat_lbl, LV_SYMBOL_CHARGE " %s %d%%", icon, pct);
  else lv_label_set_text_fmt(bat_lbl, "%s %d%%", icon, pct);
  lv_obj_set_style_text_color(bat_lbl, usb ? C_UP : (pct <= 15 ? C_DOWN : C_MUTED), 0);
}

void ui_show_main() {
  if (lv_scr_act() != scr_main) lv_scr_load(scr_main);
}

// ---------- construction ----------
static lv_obj_t *mk_button(lv_obj_t *parent, int w, int h) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, C_PANEL, 0);
  lv_obj_set_style_bg_color(b, C_ACCENT, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3240), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  return b;
}

static void build_picker() {
  picker = lv_obj_create(scr_main);
  lv_obj_set_size(picker, 640, 172);
  lv_obj_set_pos(picker, 0, 0);
  lv_obj_set_style_bg_color(picker, C_BG, 0);
  lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(picker, 0, 0);
  lv_obj_set_style_radius(picker, 0, 0);
  lv_obj_set_style_pad_all(picker, 0, 0);
  no_scroll(picker);
  lv_obj_clear_flag(picker, LV_OBJ_FLAG_GESTURE_BUBBLE);

  // grille 7 x 2 : 12 cryptos + Wi-Fi + Fermer
  const int cols = 7, gap = 6, bw = (640 - gap * (cols + 1)) / cols, bh = (172 - gap * 3) / 2;
  for (int i = 0; i < NUM_COINS + 2; i++) {
    int col = i % cols, row = i / cols;
    lv_obj_t *b = mk_button(picker, bw, bh);
    lv_obj_set_pos(b, gap + col * (bw + gap), gap + row * (bh + gap));
    if (i < NUM_COINS) {
      lv_obj_t *dot = lv_obj_create(b);
      lv_obj_set_size(dot, 10, 10);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(dot, 0, 0);
      lv_obj_set_style_bg_color(dot, lv_color_hex(COINS[i].color), 0);
      lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 12);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *l = mk_label(b, &lv_font_montserrat_20, C_TEXT, COINS[i].sym);
      lv_obj_align(l, LV_ALIGN_CENTER, 0, 6);
      lv_obj_t *n = mk_label(b, &lv_font_montserrat_12, C_MUTED, COINS[i].name);
      lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -6);
      lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
      pick_btn[i] = b;
    } else if (i == NUM_COINS) {
      lv_obj_t *l = mk_label(b, &lv_font_montserrat_20, C_MUTED, LV_SYMBOL_WIFI);
      lv_obj_align(l, LV_ALIGN_CENTER, 0, -10);
      lv_obj_t *n = mk_label(b, &lv_font_montserrat_12, C_MUTED, "Wi-Fi\n(appui long)");
      lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -4);
      lv_obj_add_event_cb(b, wifi_reset_cb, LV_EVENT_LONG_PRESSED, NULL);
    } else {
      lv_obj_t *l = mk_label(b, &lv_font_montserrat_28, C_TEXT, LV_SYMBOL_CLOSE);
      lv_obj_center(l);
      lv_obj_add_event_cb(b, close_picker_cb, LV_EVENT_CLICKED, NULL);
    }
  }
  lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
}

static void build_main() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, C_BG, 0);
  no_scroll(scr_main);
  lv_obj_add_event_cb(scr_main, gesture_cb, LV_EVENT_GESTURE, NULL);

  // --- zone en-tête cliquable (ouvre le sélecteur) ---
  lv_obj_t *hdr = lv_obj_create(scr_main);
  lv_obj_set_size(hdr, 256, 56);
  lv_obj_set_pos(hdr, 4, 2);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(hdr, C_PANEL, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 10, 0);
  lv_obj_set_style_pad_all(hdr, 0, 0);
  no_scroll(hdr);
  lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hdr, header_click_cb, LV_EVENT_CLICKED, NULL);

  badge = lv_obj_create(hdr);
  lv_obj_set_size(badge, 40, 40);
  lv_obj_set_pos(badge, 6, 8);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  no_scroll(badge);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  badge_lbl = mk_label(badge, &lv_font_montserrat_20, C_BG, "");
  lv_obj_center(badge_lbl);

  sym_lbl = mk_label(hdr, &lv_font_montserrat_28, C_TEXT, "");
  lv_obj_set_pos(sym_lbl, 54, 2);
  name_lbl = mk_label(hdr, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_set_pos(name_lbl, 55, 34);
  bat_lbl = mk_label(hdr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(bat_lbl, LV_ALIGN_TOP_RIGHT, -8, 8);
  idx_lbl = mk_label(hdr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(idx_lbl, LV_ALIGN_TOP_RIGHT, -8, 32);

  // --- prix ---
  price_lbl = mk_label(scr_main, &lv_font_montserrat_40, C_TEXT, "--");
  lv_obj_set_pos(price_lbl, 10, 62);

  chg_chip = lv_obj_create(scr_main);
  lv_obj_set_size(chg_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_pos(chg_chip, 10, 112);
  lv_obj_set_style_radius(chg_chip, 6, 0);
  lv_obj_set_style_border_width(chg_chip, 0, 0);
  lv_obj_set_style_pad_hor(chg_chip, 8, 0);
  lv_obj_set_style_pad_ver(chg_chip, 3, 0);
  no_scroll(chg_chip);
  lv_obj_clear_flag(chg_chip, LV_OBJ_FLAG_CLICKABLE);
  chg_lbl = mk_label(chg_chip, &lv_font_montserrat_16, lv_color_white(), "-- %");
  lv_obj_t *h24 = mk_label(scr_main, &lv_font_montserrat_14, C_MUTED, "24h");
  lv_obj_align_to(h24, chg_chip, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
  // repositionner "24h" quand la pastille change de taille
  lv_obj_add_event_cb(chg_chip, [](lv_event_t *e) {
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_align_to(lbl, lv_event_get_target(e), LV_ALIGN_OUT_RIGHT_MID, 8, 0);
  }, LV_EVENT_SIZE_CHANGED, h24);

  hl_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(hl_lbl, 10, 148);

  // --- séparateur ---
  lv_obj_t *sep = lv_obj_create(scr_main);
  lv_obj_set_size(sep, 1, 152);
  lv_obj_set_pos(sep, 266, 10);
  lv_obj_set_style_bg_color(sep, C_GRID, 0);
  lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_style_radius(sep, 0, 0);

  // --- ligne du haut : statut + périodes ---
  status_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(status_lbl, 278, 12);

  const int tw = 50, th = 26;
  for (int i = 0; i < NUM_TFS; i++) {
    lv_obj_t *b = mk_button(scr_main, tw, th);
    lv_obj_set_pos(b, 640 - 6 - (NUM_TFS - i) * (tw + 4), 4);
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_14, C_TEXT, TFS[i].label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, tf_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tf_btn[i] = b;
  }

  // --- graphique ---
  chart = lv_chart_create(scr_main);
  lv_obj_set_size(chart, 300, 126);
  lv_obj_set_pos(chart, 278, 38);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 4, 0);
  lv_chart_set_point_count(chart, 0);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_line_color(chart, C_GRID, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);  // pas de points
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
  series = lv_chart_add_series(chart, C_UP, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(chart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  max_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(max_lbl, 58);
  lv_obj_set_style_text_align(max_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(max_lbl, 578, 38);
  min_lbl = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(min_lbl, 58);
  lv_obj_set_style_text_align(min_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(min_lbl, 578, 150);
  chart_info = mk_label(scr_main, &lv_font_montserrat_12, C_MUTED, "Chargement...");
  lv_obj_set_width(chart_info, 58);
  lv_obj_set_style_text_align(chart_info, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(chart_info, 578, 94);

  build_picker();
}

static void build_msg() {
  scr_msg = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_msg, C_BG, 0);
  no_scroll(scr_msg);
  msg_title = mk_label(scr_msg, &lv_font_montserrat_28, C_TEXT, "");
  lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 18);
  msg_body = mk_label(scr_msg, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_set_width(msg_body, 600);
  lv_obj_set_style_text_align(msg_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 64);
}

void ui_init(int coin, int tf) {
  g_coin = (coin >= 0 && coin < NUM_COINS) ? coin : 0;
  g_tf = (tf >= 0 && tf < NUM_TFS) ? tf : 1;
  build_main();
  build_msg();
  lv_obj_add_state(tf_btn[g_tf], LV_STATE_CHECKED);
  refresh_header();
  refresh_ticker();
  ui_set_status("Demarrage", true);
  lv_scr_load(scr_msg);
}
