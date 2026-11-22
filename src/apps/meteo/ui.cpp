// Interface LVGL 8 — écran paysage 640 x 172
// Une seule page (mémoire constante) dont le contenu change quand on balaie
// vers la gauche / la droite, avec une petite animation de glissement.
//
//  0            218            434                       640
//  | ville      | Ressenti  Hum | [24 h] [7 jours]   bat  |
//  | icone 12°  | Vent  Rafales | courbe temp + pluie %   |
//  | description| Press. Nuages |   ou                    |
//  | max/min    | UV  Soleil    | 7 colonnes jour/icone   |
//  | o o o  MAJ |               |                         |
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <time.h>
#include "ui.h"

volatile int g_city = 0;
volatile int g_view = 0;
volatile bool g_reqRefresh = false;
volatile bool g_reqWifiReset = false;

// ---------- palette ----------
#define C_BG      lv_color_hex(0x0B0E14)
#define C_PANEL   lv_color_hex(0x151A23)
#define C_TEXT    lv_color_hex(0xF2F4F8)
#define C_MUTED   lv_color_hex(0x7D8696)
#define C_GRID    lv_color_hex(0x1E2430)
#define C_ACCENT  lv_color_hex(0x3B82F6)
#define C_WARM    lv_color_hex(0xFF9F43)
#define C_RAIN    lv_color_hex(0x4EA8FF)
#define C_OK      lv_color_hex(0x16C784)
#define C_BAD     lv_color_hex(0xEA3943)
#define C_SUN     lv_color_hex(0xFFC53D)
#define C_MOON    lv_color_hex(0xE8E6D9)
#define C_CLOUD   lv_color_hex(0xC9D1DC)
#define C_CLOUD2  lv_color_hex(0x8A94A6)
#define C_CLOUD3  lv_color_hex(0x5F6878)

// ---------- cache ----------
struct CityState {
  char name[40], region[48], err[40];
  bool hasWx;
  uint32_t fetchedMs;
  WxData wx;
};
static CityState s_city[MAX_CITIES];
static int s_n = 1;

// ---------- objets ----------
static lv_obj_t *scr_main, *scr_msg, *page, *settings, *info_lbl;
static lv_obj_t *city_lbl, *sub_lbl, *icon_now, *temp_lbl, *desc_lbl, *minmax_lbl, *status_lbl, *bat_lbl;
static lv_obj_t *dots[MAX_CITIES];
static lv_obj_t *cell_val[8];
static lv_obj_t *tab_btn[2], *view24, *view7;
static lv_obj_t *chart_t, *chart_p, *pt_lbl[6], *hr_lbl[6], *empty24, *empty7;
static lv_chart_series_t *ser_t, *ser_p;
static lv_obj_t *d_day[WX_DAYS], *d_icon[WX_DAYS], *d_max[WX_DAYS], *d_min[WX_DAYS], *d_pop[WX_DAYS], *d_bar[WX_DAYS];
static lv_obj_t *msg_title, *msg_body;
static char s_status[48] = "";
static bool s_statusOk = true;

static const char *CELL_NAMES[8] = {"Ressenti", "Humidite", "Vent km/h", "Rafales km/h",
                                     "Pression hPa", "Nuages", "Indice UV max", "Lever  /  coucher"};

// ---------- textes ----------
static const char *wx_desc(int code, bool day) {
  switch (code) {
    case 0:  return day ? "Ciel degage" : "Nuit claire";
    case 1:  return "Plutot degage";
    case 2:  return "Partiellement nuageux";
    case 3:  return "Couvert";
    case 45: return "Brouillard";
    case 48: return "Brouillard givrant";
    case 51: return "Bruine legere";
    case 53: return "Bruine";
    case 55: return "Bruine dense";
    case 56: case 57: return "Bruine verglacante";
    case 61: return "Pluie faible";
    case 63: return "Pluie moderee";
    case 65: return "Pluie forte";
    case 66: case 67: return "Pluie verglacante";
    case 71: return "Neige faible";
    case 73: return "Neige moderee";
    case 75: return "Neige forte";
    case 77: return "Grains de neige";
    case 80: return "Averses faibles";
    case 81: return "Averses";
    case 82: return "Averses violentes";
    case 85: return "Averses de neige";
    case 86: return "Fortes averses de neige";
    case 95: return "Orage";
    case 96: return "Orage et grele";
    case 99: return "Orage violent et grele";
    default: return "--";
  }
}

static const char *wind_dir(int deg) {
  static const char *d[8] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
  return d[((deg % 360 + 360) % 360 + 22) / 45 % 8];
}

static const char *DAYS[7] = {"Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam"};

static void deg(char *out, size_t n, float v) { snprintf(out, n, "%d\xC2\xB0", (int)lroundf(v)); }

static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

// ---------- icônes dessinées (aucune image embarquée) ----------
enum Kind { K_CLEAR, K_FEW, K_CLOUD, K_OVERCAST, K_FOG, K_DRIZZLE, K_RAIN, K_HEAVY, K_SNOW, K_SLEET, K_STORM };

static Kind kind_of(int code) {
  switch (code) {
    case 0: return K_CLEAR;
    case 1: case 2: return K_FEW;
    case 3: return K_OVERCAST;
    case 45: case 48: return K_FOG;
    case 51: case 53: case 55: return K_DRIZZLE;
    case 56: case 57: case 66: case 67: return K_SLEET;
    case 61: case 63: case 80: case 81: return K_RAIN;
    case 65: case 82: return K_HEAVY;
    case 71: case 73: case 75: case 77: case 85: case 86: return K_SNOW;
    case 95: case 96: case 99: return K_STORM;
    default: return K_CLOUD;
  }
}

static void d_circle(lv_draw_ctx_t *ctx, int cx, int cy, int r, lv_color_t c) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = c;
  d.bg_opa = LV_OPA_COVER;
  lv_area_t a = {(lv_coord_t)(cx - r), (lv_coord_t)(cy - r), (lv_coord_t)(cx + r), (lv_coord_t)(cy + r)};
  lv_draw_rect(ctx, &d, &a);
}

static void d_line(lv_draw_ctx_t *ctx, int x1, int y1, int x2, int y2, int w, lv_color_t c) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.width = w;
  d.color = c;
  d.round_start = d.round_end = 1;
  lv_point_t p1 = {(lv_coord_t)x1, (lv_coord_t)y1}, p2 = {(lv_coord_t)x2, (lv_coord_t)y2};
  lv_draw_line(ctx, &d, &p1, &p2);
}

static void d_sun(lv_draw_ctx_t *ctx, int cx, int cy, int r) {
  int w = LV_MAX(2, r / 4);
  for (int i = 0; i < 8; i++) {
    float a = i * (float)M_PI / 4;
    d_line(ctx, cx + cosf(a) * r * 1.4f, cy + sinf(a) * r * 1.4f,
           cx + cosf(a) * r * 1.8f, cy + sinf(a) * r * 1.8f, w, C_SUN);
  }
  d_circle(ctx, cx, cy, r, C_SUN);
}

// croissant de lune : disque dont on masque un disque décalé
static void d_moon(lv_draw_ctx_t *ctx, int cx, int cy, int r) {
  lv_area_t cut = {(lv_coord_t)(cx - r * 0.35f), (lv_coord_t)(cy - r * 1.35f),
                   (lv_coord_t)(cx + r * 1.55f), (lv_coord_t)(cy + r * 0.55f)};
  lv_draw_mask_radius_param_t m;
  lv_draw_mask_radius_init(&m, &cut, LV_RADIUS_CIRCLE, true);
  int16_t id = lv_draw_mask_add(&m, NULL);
  d_circle(ctx, cx, cy, r, C_MOON);
  lv_draw_mask_free_param(&m);
  lv_draw_mask_remove_id(id);
}

// nuage : base arrondie + deux bosses ; L = gauche, W = largeur, B = bas
static void d_cloud(lv_draw_ctx_t *ctx, int L, int W, int B, lv_color_t c) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = c;
  d.bg_opa = LV_OPA_COVER;
  lv_area_t a = {(lv_coord_t)L, (lv_coord_t)(B - W * 0.32f), (lv_coord_t)(L + W), (lv_coord_t)B};
  lv_draw_rect(ctx, &d, &a);
  d_circle(ctx, L + W * 0.30f, B - W * 0.30f, W * 0.20f, c);
  d_circle(ctx, L + W * 0.60f, B - W * 0.36f, W * 0.27f, c);
}

static void icon_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  uint32_t ud = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
  if (!(ud & 0x10000)) return;  // pas encore de données
  int code = ud & 0xFF;
  bool day = ud & 0x100;
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  int x = a.x1, y = a.y1, s = lv_area_get_width(&a);
  int lw = LV_MAX(2, s / 22);
  Kind k = kind_of(code);

  auto X = [&](float f) { return (int)(x + f * s); };
  auto Y = [&](float f) { return (int)(y + f * s); };

  switch (k) {
    case K_CLEAR:
      if (day) d_sun(ctx, X(.5f), Y(.5f), s * .22f);
      else d_moon(ctx, X(.46f), Y(.54f), s * .30f);
      return;
    case K_FEW:
      if (day) d_sun(ctx, X(.38f), Y(.38f), s * .16f);
      else d_moon(ctx, X(.36f), Y(.38f), s * .20f);
      d_cloud(ctx, X(.24f), s * .68f, Y(.82f), C_CLOUD);
      return;
    case K_CLOUD:
      d_cloud(ctx, X(.10f), s * .80f, Y(.76f), C_CLOUD);
      return;
    case K_OVERCAST:
      d_cloud(ctx, X(.34f), s * .58f, Y(.58f), C_CLOUD2);
      d_cloud(ctx, X(.06f), s * .74f, Y(.80f), C_CLOUD);
      return;
    case K_FOG:
      d_cloud(ctx, X(.14f), s * .72f, Y(.56f), C_CLOUD2);
      d_line(ctx, X(.12f), Y(.68f), X(.88f), Y(.68f), lw, C_CLOUD);
      d_line(ctx, X(.20f), Y(.80f), X(.80f), Y(.80f), lw, C_CLOUD);
      d_line(ctx, X(.12f), Y(.92f), X(.70f), Y(.92f), lw, C_CLOUD);
      return;
    default: break;
  }

  // précipitations : nuage en haut, gouttes / flocons / éclair en dessous
  bool shower = code >= 80 && code <= 86;
  if (shower && day) d_sun(ctx, X(.30f), Y(.26f), s * .13f);
  lv_color_t cc = (k == K_HEAVY || k == K_STORM) ? C_CLOUD3 : C_CLOUD2;
  d_cloud(ctx, X(.10f), s * .80f, Y(.60f), cc);
  const float xs[3] = {.32f, .50f, .68f};
  switch (k) {
    case K_DRIZZLE:
      for (int i = 0; i < 3; i++) d_line(ctx, X(xs[i]), Y(.70f), X(xs[i] - .02f), Y(.76f), lw, C_RAIN);
      for (int i = 0; i < 2; i++) d_line(ctx, X(xs[i] + .08f), Y(.84f), X(xs[i] + .06f), Y(.90f), lw, C_RAIN);
      break;
    case K_RAIN:
      for (int i = 0; i < 3; i++) d_line(ctx, X(xs[i]), Y(.70f), X(xs[i] - .06f), Y(.90f), lw, C_RAIN);
      break;
    case K_HEAVY:
      for (int i = 0; i < 4; i++) {
        float fx = .26f + i * .16f;
        d_line(ctx, X(fx), Y(.68f), X(fx - .08f), Y(.96f), lw, C_RAIN);
      }
      break;
    case K_SNOW:
      for (int i = 0; i < 3; i++) d_circle(ctx, X(xs[i]), Y(i == 1 ? .86f : .76f), LV_MAX(1, s / 20), C_TEXT);
      d_circle(ctx, X(.41f), Y(.94f), LV_MAX(1, s / 22), C_TEXT);
      d_circle(ctx, X(.59f), Y(.94f), LV_MAX(1, s / 22), C_TEXT);
      break;
    case K_SLEET:
      d_line(ctx, X(.34f), Y(.70f), X(.30f), Y(.84f), lw, C_RAIN);
      d_circle(ctx, X(.50f), Y(.80f), LV_MAX(1, s / 20), C_TEXT);
      d_line(ctx, X(.68f), Y(.70f), X(.64f), Y(.84f), lw, C_RAIN);
      d_circle(ctx, X(.44f), Y(.94f), LV_MAX(1, s / 22), C_TEXT);
      break;
    case K_STORM: {
      int bw = LV_MAX(2, s / 16);
      d_line(ctx, X(.56f), Y(.58f), X(.42f), Y(.80f), bw, C_SUN);
      d_line(ctx, X(.42f), Y(.80f), X(.58f), Y(.80f), bw, C_SUN);
      d_line(ctx, X(.58f), Y(.80f), X(.46f), Y(.98f), bw, C_SUN);
      d_line(ctx, X(.28f), Y(.70f), X(.24f), Y(.82f), lw, C_RAIN);
      d_line(ctx, X(.76f), Y(.70f), X(.72f), Y(.82f), lw, C_RAIN);
      break;
    }
    default: break;
  }
}

static lv_obj_t *mk_icon(lv_obj_t *parent, int size) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, size, size);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
  return o;
}

static void icon_set(lv_obj_t *o, int code, bool day) {
  lv_obj_set_user_data(o, (void *)(uintptr_t)(0x10000 | (day ? 0x100 : 0) | (code & 0xFF)));
  lv_obj_invalidate(o);
}

static void icon_clear(lv_obj_t *o) {
  lv_obj_set_user_data(o, NULL);
  lv_obj_invalidate(o);
}

// teinte de fond (côté gauche) selon le temps qu'il fait
static lv_color_t tint_of(const WxData &w) {
  if (!w.isDay) return lv_color_hex(0x141B38);
  switch (kind_of(w.code)) {
    case K_CLEAR: case K_FEW: return lv_color_hex(0x1C3F66);
    case K_STORM: return lv_color_hex(0x2C2347);
    case K_SNOW: case K_SLEET: return lv_color_hex(0x2B3A4D);
    case K_RAIN: case K_HEAVY: case K_DRIZZLE: return lv_color_hex(0x1A2B40);
    default: return lv_color_hex(0x252E3B);
  }
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
  lv_obj_set_style_bg_color(b, C_ACCENT, LV_STATE_CHECKED);
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

static lv_obj_t *mk_sep(lv_obj_t *parent, int x) {
  lv_obj_t *s = mk_box(parent, x, 10, 1, 152);
  lv_obj_set_style_bg_color(s, C_GRID, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  return s;
}

// Remplissage dégradé sous la courbe (comme CryptoTracker / lv_example_chart_2)
static void chart_fill_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || !dsc->p1 || !dsc->p2) return;
  lv_draw_mask_line_param_t lm;
  lv_draw_mask_line_points_init(&lm, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y, LV_DRAW_MASK_LINE_SIDE_BOTTOM);
  int16_t lid = lv_draw_mask_add(&lm, NULL);
  lv_draw_mask_fade_param_t fm;
  lv_draw_mask_fade_init(&fm, &obj->coords, LV_OPA_COVER, obj->coords.y1, LV_OPA_TRANSP, obj->coords.y2);
  int16_t fid = lv_draw_mask_add(&fm, NULL);
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
  lv_draw_mask_free_param(&lm);
  lv_draw_mask_free_param(&fm);
  lv_draw_mask_remove_id(lid);
  lv_draw_mask_remove_id(fid);
}

// ---------- rafraîchissement de la page ----------
static void refresh_status();
static void refresh_clock();

static void refresh_view_tabs() {
  for (int i = 0; i < 2; i++) {
    if (i == g_view) lv_obj_add_state(tab_btn[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(tab_btn[i], LV_STATE_CHECKED);
  }
  if (g_view == 0) { lv_obj_clear_flag(view24, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(view7, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(view24, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(view7, LV_OBJ_FLAG_HIDDEN); }
}

static void refresh_dots() {
  for (int i = 0; i < MAX_CITIES; i++) {
    if (i >= s_n || s_n < 2) { lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    bool cur = i == g_city;
    lv_obj_set_size(dots[i], cur ? 16 : 6, 6);
    lv_obj_set_style_bg_color(dots[i], cur ? C_TEXT : C_MUTED, 0);
    lv_obj_set_style_bg_opa(dots[i], cur ? LV_OPA_COVER : LV_OPA_60, 0);
  }
}

static void refresh_24(const WxData &w) {
  int n = w.nh;
  if (n < 2) {
    lv_chart_set_point_count(chart_t, 0);
    lv_chart_set_point_count(chart_p, 0);
    for (int i = 0; i < 6; i++) { lv_obj_add_flag(pt_lbl[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(hr_lbl[i], LV_OBJ_FLAG_HIDDEN); }
    lv_obj_clear_flag(empty24, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(empty24, LV_OBJ_FLAG_HIDDEN);
  float mn = w.hTemp[0], mx = w.hTemp[0];
  for (int i = 1; i < n; i++) { mn = fminf(mn, w.hTemp[i]); mx = fmaxf(mx, w.hTemp[i]); }
  float span = fmaxf(mx - mn, 2.0f);
  // marge en haut pour les étiquettes de température
  int lo = (int)floorf((mn - span * 0.10f) * 10), hi = (int)ceilf((mx + span * 0.55f) * 10);
  lv_chart_set_point_count(chart_t, n);
  lv_chart_set_range(chart_t, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  lv_chart_set_point_count(chart_p, n);
  for (int i = 0; i < n; i++) {
    ser_t->y_points[i] = (lv_coord_t)lroundf(w.hTemp[i] * 10);
    ser_p->y_points[i] = w.hPop[i];
  }
  lv_chart_refresh(chart_t);
  lv_chart_refresh(chart_p);

  lv_coord_t cw = lv_obj_get_width(chart_t), ch = lv_obj_get_height(chart_t);
  lv_coord_t cx = lv_obj_get_x(chart_t), cy = lv_obj_get_y(chart_t);
  for (int k = 0; k < 6; k++) {
    int i = k * 4 + 1;
    if (i >= n) { lv_obj_add_flag(pt_lbl[k], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(hr_lbl[k], LV_OBJ_FLAG_HIDDEN); continue; }
    int px = cx + (cw - 1) * i / (n - 1);
    int py = cy + ch - (int)((w.hTemp[i] * 10 - lo) * ch / (float)(hi - lo));
    char buf[12];
    deg(buf, sizeof(buf), w.hTemp[i]);
    lv_label_set_text(pt_lbl[k], buf);
    lv_obj_set_pos(pt_lbl[k], px - 16, py - 18);
    lv_obj_clear_flag(pt_lbl[k], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(hr_lbl[k], "%dh", (w.startHour + i) % 24);
    lv_obj_set_pos(hr_lbl[k], px - 16, cy + ch + 3);
    lv_obj_clear_flag(hr_lbl[k], LV_OBJ_FLAG_HIDDEN);
  }
}

static void refresh_7(const WxData &w) {
  if (w.nd < 1) lv_obj_clear_flag(empty7, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(empty7, LV_OBJ_FLAG_HIDDEN);
  float mn = 1e9, mx = -1e9;
  for (int i = 0; i < w.nd; i++) { mn = fminf(mn, w.d[i].tmin); mx = fmaxf(mx, w.d[i].tmax); }
  float span = fmaxf(mx - mn, 1.0f);
  const int barTop = 80, barH = 40;
  char buf[12];
  for (int i = 0; i < WX_DAYS; i++) {
    bool on = i < w.nd;
    lv_obj_t *objs[] = {d_day[i], d_icon[i], d_max[i], d_min[i], d_pop[i], d_bar[i]};
    for (lv_obj_t *o : objs) on ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (!on) continue;
    const WxDay &d = w.d[i];
    lv_label_set_text(d_day[i], i == 0 ? "Auj" : DAYS[d.wday % 7]);
    icon_set(d_icon[i], d.code, true);
    deg(buf, sizeof(buf), d.tmax); lv_label_set_text(d_max[i], buf);
    deg(buf, sizeof(buf), d.tmin); lv_label_set_text(d_min[i], buf);
    if (d.pop >= 10) lv_label_set_text_fmt(d_pop[i], "%d%%", d.pop);
    else lv_label_set_text(d_pop[i], "");
    int y1 = barTop + (int)((mx - d.tmax) / span * barH);
    int y2 = barTop + (int)((mx - d.tmin) / span * barH);
    if (y2 - y1 < 4) y2 = y1 + 4;
    lv_obj_set_y(d_bar[i], y1);
    lv_obj_set_height(d_bar[i], y2 - y1);
  }
}

static void refresh_page() {
  const CityState &c = s_city[g_city];
  lv_label_set_text(city_lbl, c.name);
  refresh_dots();
  refresh_view_tabs();

  if (!c.hasWx) {
    lv_label_set_text(temp_lbl, "--");
    lv_label_set_text(desc_lbl, c.err[0] ? c.err : "Chargement...");
    lv_obj_set_style_text_color(desc_lbl, c.err[0] ? C_BAD : C_MUTED, 0);
    lv_label_set_text(minmax_lbl, "");
    icon_clear(icon_now);
    for (int i = 0; i < 8; i++) lv_label_set_text(cell_val[i], "--");
    WxData empty = {};
    refresh_24(empty);
    refresh_7(empty);
    for (int i = 0; i < WX_DAYS; i++) icon_clear(d_icon[i]);
    lv_obj_set_style_bg_grad_color(page, C_BG, 0);
    lv_obj_set_style_bg_color(page, C_BG, 0);
    refresh_clock();
    refresh_status();
    return;
  }

  const WxData &w = c.wx;
  char buf[48], a[12], b[12];
  lv_obj_set_style_bg_color(page, tint_of(w), 0);
  lv_obj_set_style_bg_grad_color(page, C_BG, 0);

  deg(buf, sizeof(buf), w.temp);
  lv_label_set_text(temp_lbl, buf);
  icon_set(icon_now, w.code, w.isDay);
  lv_label_set_text(desc_lbl, wx_desc(w.code, w.isDay));
  lv_obj_set_style_text_color(desc_lbl, C_TEXT, 0);
  if (w.nd > 0) {
    deg(a, sizeof(a), w.d[0].tmax);
    deg(b, sizeof(b), w.d[0].tmin);
    char mm[12];
    snprintf(mm, sizeof(mm), "%.1f", w.d[0].rain);
    comma(mm);
    if (w.d[0].rain >= 0.1f) lv_label_set_text_fmt(minmax_lbl, "Max %s  Min %s  " "#4EA8FF %s mm#", a, b, mm);
    else lv_label_set_text_fmt(minmax_lbl, "Max %s  Min %s  Sec", a, b);
  } else {
    lv_label_set_text(minmax_lbl, "");
  }

  deg(buf, sizeof(buf), w.feels);                       lv_label_set_text(cell_val[0], buf);
  lv_label_set_text_fmt(cell_val[1], "%d %%", w.hum);
  lv_label_set_text_fmt(cell_val[2], "%d %s", (int)lroundf(w.wind), wind_dir(w.windDir));
  lv_label_set_text_fmt(cell_val[3], "%d", (int)lroundf(w.gust));
  lv_label_set_text_fmt(cell_val[4], "%d", (int)lroundf(w.pressure));
  lv_label_set_text_fmt(cell_val[5], "%d %%", w.cloud);
  lv_obj_set_style_text_color(cell_val[3], w.gust >= 60 ? C_BAD : C_TEXT, 0);
  if (w.nd > 0) {
    float uv = w.d[0].uv;
    const char *lvl = uv < 3 ? "faible" : uv < 6 ? "modere" : uv < 8 ? "eleve" : uv < 11 ? "tres eleve" : "extreme";
    uint32_t col = uv < 3 ? 0x16C784 : uv < 6 ? 0xFFC53D : uv < 8 ? 0xFF9F43 : uv < 11 ? 0xEA3943 : 0xB26BFF;
    lv_label_set_text_fmt(cell_val[6], "%d %s", (int)lroundf(uv), lvl);
    lv_obj_set_style_text_color(cell_val[6], lv_color_hex(col), 0);
    lv_label_set_text_fmt(cell_val[7], "%s  %s", w.d[0].rise, w.d[0].set);
  } else {
    lv_label_set_text(cell_val[6], "--");
    lv_label_set_text(cell_val[7], "--");
  }

  refresh_24(w);
  refresh_7(w);
  refresh_clock();
  refresh_status();
}

// heure locale de la ville (si l'heure NTP est connue) à côté de la région
static void refresh_clock() {
  const CityState &c = s_city[g_city];
  time_t now = time(NULL);
  if (c.hasWx && now > 1700000000) {
    time_t t = now + c.wx.utcOffset;
    struct tm tm;
    gmtime_r(&t, &tm);
    if (c.region[0]) lv_label_set_text_fmt(sub_lbl, "%s  -  %02d:%02d", c.region, tm.tm_hour, tm.tm_min);
    else lv_label_set_text_fmt(sub_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
  } else {
    lv_label_set_text(sub_lbl, c.region);
  }
}

static void refresh_status() {
  const CityState &c = s_city[g_city];
  if (!s_statusOk || !c.hasWx) {
    lv_label_set_text(status_lbl, s_status);
    lv_obj_set_style_text_color(status_lbl, s_statusOk ? C_MUTED : C_BAD, 0);
    return;
  }
  uint32_t min = (millis() - c.fetchedMs) / 60000;
  if (min == 0) lv_label_set_text(status_lbl, LV_SYMBOL_REFRESH " a l'instant");
  else if (min < 60) lv_label_set_text_fmt(status_lbl, LV_SYMBOL_REFRESH " il y a %u min", (unsigned)min);
  else lv_label_set_text_fmt(status_lbl, LV_SYMBOL_REFRESH " il y a %u h", (unsigned)(min / 60));
  lv_obj_set_style_text_color(status_lbl, min >= 30 ? C_WARM : C_MUTED, 0);
}

static void tick_cb(lv_timer_t *) {
  refresh_clock();
  refresh_status();
}

// ---------- changement de ville (animation de glissement) ----------
static int s_animDir = 0;
static bool s_animating = false;

static void anim_x(void *o, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0);
  lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)LV_MAX(0, 255 - abs((int)v) * 4), 0);
}

static void anim_in_done(lv_anim_t *) { s_animating = false; }

static void anim_out_done(lv_anim_t *) {
  g_city = (g_city + s_animDir + s_n) % s_n;
  refresh_page();
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  lv_anim_set_exec_cb(&a, anim_x);
  lv_anim_set_values(&a, s_animDir * 60, 0);
  lv_anim_set_time(&a, 170);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, anim_in_done);
  lv_anim_start(&a);
}

static void go_city(int dir) {
  if (s_n < 2 || s_animating) return;
  s_animating = true;
  s_animDir = dir;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  lv_anim_set_exec_cb(&a, anim_x);
  lv_anim_set_values(&a, 0, -dir * 60);
  lv_anim_set_time(&a, 110);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, anim_out_done);
  lv_anim_start(&a);
}

// ---------- événements ----------
static void gesture_cb(lv_event_t *e) {
  if (!lv_obj_has_flag(settings, LV_OBJ_FLAG_HIDDEN)) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) go_city(+1);
  else if (dir == LV_DIR_RIGHT) go_city(-1);
  else return;
  lv_indev_wait_release(lv_indev_get_act());
}

static void tab_cb(lv_event_t *e) {
  g_view = (int)(intptr_t)lv_event_get_user_data(e);
  refresh_view_tabs();
}

static void open_settings_cb(lv_event_t *) {
  lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(settings);
}

static void close_settings_cb(lv_event_t *) { lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); }

static void refresh_cb(lv_event_t *) {
  g_reqRefresh = true;
  lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_reset_cb(lv_event_t *) { g_reqWifiReset = true; }

// ---------- construction ----------
static void build_col_a() {
  city_lbl = mk_label(page, &lv_font_montserrat_20, C_TEXT, "");
  lv_obj_set_pos(city_lbl, 12, 6);
  lv_obj_set_width(city_lbl, 176);
  lv_label_set_long_mode(city_lbl, LV_LABEL_LONG_DOT);

  // engrenage : ouvre les réglages (actualiser, Wi-Fi, adresse de configuration)
  lv_obj_t *gear = lv_btn_create(page);
  lv_obj_remove_style_all(gear);
  lv_obj_set_size(gear, 34, 30);
  lv_obj_set_pos(gear, 182, 2);
  lv_obj_set_style_radius(gear, 8, 0);
  lv_obj_set_style_bg_color(gear, C_PANEL, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *gl = mk_label(gear, &lv_font_montserrat_16, C_MUTED, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);
  lv_obj_add_event_cb(gear, open_settings_cb, LV_EVENT_CLICKED, NULL);

  sub_lbl = mk_label(page, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(sub_lbl, 12, 30);
  lv_obj_set_width(sub_lbl, 200);
  lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);

  icon_now = mk_icon(page, 62);
  lv_obj_set_pos(icon_now, 8, 48);
  temp_lbl = mk_label(page, &lv_font_montserrat_40, C_TEXT, "--");
  lv_obj_set_pos(temp_lbl, 78, 56);

  desc_lbl = mk_label(page, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_set_pos(desc_lbl, 12, 112);
  lv_obj_set_width(desc_lbl, 200);
  lv_label_set_long_mode(desc_lbl, LV_LABEL_LONG_DOT);

  minmax_lbl = mk_label(page, &lv_font_montserrat_12, C_MUTED, "");
  lv_label_set_recolor(minmax_lbl, true);
  lv_obj_set_pos(minmax_lbl, 12, 132);

  for (int i = 0; i < MAX_CITIES; i++) {
    dots[i] = mk_box(page, 0, 0, 6, 6);
    lv_obj_set_style_radius(dots[i], 3, 0);
  }
  // les points s'alignent tout seuls : on les place dans un conteneur flex
  lv_obj_t *row = mk_box(page, 12, 155, 120, 8);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 5, 0);
  for (int i = 0; i < MAX_CITIES; i++) lv_obj_set_parent(dots[i], row);

  status_lbl = mk_label(page, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(status_lbl, 120);
  lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(status_lbl, 92, 152);
}

static void build_col_b() {
  for (int i = 0; i < 8; i++) {
    int x = 228 + (i % 2) * 102, y = 6 + (i / 2) * 41;
    lv_obj_t *l = mk_label(page, &lv_font_montserrat_12, C_MUTED, CELL_NAMES[i]);
    lv_obj_set_pos(l, x, y);
    cell_val[i] = mk_label(page, i == 7 ? &lv_font_montserrat_14 : &lv_font_montserrat_16, C_TEXT, "--");
    lv_obj_set_pos(cell_val[i], x, y + 16);
  }
}

static void build_col_c() {
  const int X = 442, W = 192;
  const char *tabs[2] = {"24 h", "7 jours"};
  for (int i = 0; i < 2; i++) {
    tab_btn[i] = mk_button(page, i == 0 ? 52 : 70, 24, tabs[i]);
    lv_obj_set_pos(tab_btn[i], X + i * 56, 4);
    lv_obj_add_event_cb(tab_btn[i], tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  bat_lbl = mk_label(page, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(bat_lbl, 64);
  lv_obj_set_style_text_align(bat_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(bat_lbl, 640 - 6 - 64, 9);

  // --- vue 24 h : barres de risque de pluie + courbe de température ---
  view24 = mk_box(page, X, 32, W, 140);
  chart_p = lv_chart_create(view24);
  lv_obj_set_size(chart_p, W, 90);
  lv_obj_set_pos(chart_p, 0, 0);
  lv_chart_set_type(chart_p, LV_CHART_TYPE_BAR);
  lv_chart_set_range(chart_p, LV_CHART_AXIS_PRIMARY_Y, 0, 220);  // 100 % = ~45 % de la hauteur
  lv_chart_set_div_line_count(chart_p, 0, 0);
  lv_chart_set_point_count(chart_p, 0);
  lv_obj_set_style_bg_opa(chart_p, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart_p, 0, 0);
  lv_obj_set_style_pad_all(chart_p, 0, 0);
  lv_obj_set_style_pad_column(chart_p, 2, 0);
  lv_obj_set_style_radius(chart_p, 2, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(chart_p, LV_OPA_60, LV_PART_ITEMS);
  lv_obj_clear_flag(chart_p, LV_OBJ_FLAG_CLICKABLE);
  ser_p = lv_chart_add_series(chart_p, C_RAIN, LV_CHART_AXIS_PRIMARY_Y);

  chart_t = lv_chart_create(view24);
  lv_obj_set_size(chart_t, W, 90);
  lv_obj_set_pos(chart_t, 0, 0);
  lv_chart_set_type(chart_t, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart_t, 3, 0);
  lv_chart_set_point_count(chart_t, 0);
  lv_obj_set_style_bg_opa(chart_t, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart_t, 0, 0);
  lv_obj_set_style_pad_all(chart_t, 0, 0);
  lv_obj_set_style_line_color(chart_t, C_GRID, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart_t, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(chart_t, 0, LV_PART_INDICATOR);
  lv_obj_clear_flag(chart_t, LV_OBJ_FLAG_CLICKABLE);
  ser_t = lv_chart_add_series(chart_t, C_WARM, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(chart_t, chart_fill_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  for (int k = 0; k < 6; k++) {
    pt_lbl[k] = mk_label(view24, &lv_font_montserrat_12, C_TEXT, "");
    lv_obj_set_width(pt_lbl[k], 32);
    lv_obj_set_style_text_align(pt_lbl[k], LV_TEXT_ALIGN_CENTER, 0);
    hr_lbl[k] = mk_label(view24, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_set_width(hr_lbl[k], 32);
    lv_obj_set_style_text_align(hr_lbl[k], LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_obj_t *legend = mk_label(view24, &lv_font_montserrat_12, C_MUTED, "#FF9F43 Temperature#    #4EA8FF Risque de pluie#");
  lv_label_set_recolor(legend, true);
  lv_obj_set_pos(legend, 0, 120);
  empty24 = mk_label(view24, &lv_font_montserrat_14, C_MUTED, "Chargement...");
  lv_obj_align(empty24, LV_ALIGN_TOP_MID, 0, 34);

  // --- vue 7 jours ---
  view7 = mk_box(page, X, 32, W, 140);
  const int cw = W / WX_DAYS;
  for (int i = 0; i < WX_DAYS; i++) {
    int x = i * cw;
    d_day[i] = mk_label(view7, &lv_font_montserrat_12, C_MUTED, "");
    d_icon[i] = mk_icon(view7, 24);
    d_max[i] = mk_label(view7, &lv_font_montserrat_14, C_TEXT, "");
    d_min[i] = mk_label(view7, &lv_font_montserrat_12, C_MUTED, "");
    d_pop[i] = mk_label(view7, &lv_font_montserrat_12, C_RAIN, "");
    lv_obj_t *lbls[] = {d_day[i], d_max[i], d_min[i], d_pop[i]};
    const int ys[] = {0, 42, 60, 124};
    for (int j = 0; j < 4; j++) {
      lv_obj_set_width(lbls[j], cw + 6);
      lv_obj_set_style_text_align(lbls[j], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(lbls[j], x - 3, ys[j]);
    }
    lv_obj_set_pos(d_icon[i], x + (cw - 24) / 2, 16);
    // rail + barre min..max (échelle commune à la semaine)
    lv_obj_t *rail = mk_box(view7, x + cw / 2 - 1, 80, 2, 40);
    lv_obj_set_style_bg_color(rail, C_GRID, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    d_bar[i] = mk_box(view7, x + cw / 2 - 3, 80, 6, 10);
    lv_obj_set_style_radius(d_bar[i], 3, 0);
    lv_obj_set_style_bg_opa(d_bar[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d_bar[i], C_WARM, 0);
    lv_obj_set_style_bg_grad_color(d_bar[i], C_RAIN, 0);
    lv_obj_set_style_bg_grad_dir(d_bar[i], LV_GRAD_DIR_VER, 0);
  }
  empty7 = mk_label(view7, &lv_font_montserrat_14, C_MUTED, "Chargement...");
  lv_obj_align(empty7, LV_ALIGN_TOP_MID, 0, 34);
}

static void build_settings() {
  settings = lv_obj_create(scr_main);
  lv_obj_remove_style_all(settings);
  lv_obj_set_size(settings, 640, 172);
  lv_obj_set_style_bg_color(settings, C_BG, 0);
  lv_obj_set_style_bg_opa(settings, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settings, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
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

static void build_main() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, C_BG, 0);
  lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_main, gesture_cb, LV_EVENT_GESTURE, NULL);

  page = lv_obj_create(scr_main);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, 640, 172);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(page, C_BG, 0);
  lv_obj_set_style_bg_grad_color(page, C_BG, 0);
  lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_bg_main_stop(page, 0, 0);
  lv_obj_set_style_bg_grad_stop(page, 110, 0);  // la teinte s'estompe vers le tiers gauche
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  build_col_a();
  mk_sep(page, 218);
  build_col_b();
  mk_sep(page, 434);
  build_col_c();
  build_settings();
  lv_timer_create(tick_cb, 15000, NULL);
}

static void build_msg() {
  scr_msg = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_msg, C_BG, 0);
  lv_obj_clear_flag(scr_msg, LV_OBJ_FLAG_SCROLLABLE);
  msg_title = mk_label(scr_msg, &lv_font_montserrat_28, C_TEXT, "");
  lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 18);
  msg_body = mk_label(scr_msg, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_set_width(msg_body, 600);
  lv_obj_set_style_text_align(msg_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 64);
}

// ---------- API publique ----------
void ui_init(int n, const char *const *names, int city, int view) {
  s_n = LV_MAX(1, LV_MIN(n, MAX_CITIES));
  for (int i = 0; i < s_n; i++) {
    memset(&s_city[i], 0, sizeof(CityState));
    snprintf(s_city[i].name, sizeof(s_city[i].name), "%s", names[i]);
  }
  g_city = (city >= 0 && city < s_n) ? city : 0;
  g_view = (view == 1) ? 1 : 0;
  build_main();
  build_msg();
  snprintf(s_status, sizeof(s_status), "Demarrage");
  refresh_page();
  lv_scr_load(scr_msg);
}

void ui_set_city(int i, const char *name, const char *region) {
  if (i < 0 || i >= s_n) return;
  snprintf(s_city[i].name, sizeof(s_city[i].name), "%s", name);
  snprintf(s_city[i].region, sizeof(s_city[i].region), "%s", region);
  s_city[i].err[0] = 0;
  if (i == g_city && !s_animating) refresh_page();
}

void ui_set_city_error(int i, const char *msg) {
  if (i < 0 || i >= s_n) return;
  snprintf(s_city[i].err, sizeof(s_city[i].err), "%s", msg);
  if (i == g_city && !s_animating) refresh_page();
}

void ui_set_weather(int i, const WxData &d) {
  if (i < 0 || i >= s_n) return;
  s_city[i].wx = d;
  s_city[i].hasWx = true;
  s_city[i].err[0] = 0;
  s_city[i].fetchedMs = millis();
  if (i == g_city && !s_animating) refresh_page();
}

void ui_set_status(const char *txt, bool ok) {
  snprintf(s_status, sizeof(s_status), "%s %s", ok ? "" : LV_SYMBOL_WARNING, txt);
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
  if (usb) lv_label_set_text_fmt(bat_lbl, LV_SYMBOL_CHARGE " %s %d%%", icon, pct);
  else lv_label_set_text_fmt(bat_lbl, "%s %d%%", icon, pct);
  lv_obj_set_style_text_color(bat_lbl, usb ? C_OK : (pct <= 15 ? C_BAD : C_MUTED), 0);
}
