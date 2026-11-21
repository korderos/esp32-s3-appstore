#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "../../display.h"

// App « Dessin » : ardoise tactile, barre d'outils en haut, toile en PSRAM dessous.

#define TOOLBAR_H 92
#define CANVAS_W_PX 172
#define CANVAS_H  (640 - TOOLBAR_H)

static lv_obj_t *canvas;
static lv_color_t *canvas_buf;
static lv_color_t pen_color;
static lv_coord_t pen_width = 6;
static lv_point_t last_pt;
static bool has_last = false;

static const uint32_t palette[] = {0xFFFFFF, 0xFF3B30, 0xFFCC00, 0x34C759, 0x0A84FF, 0xBF5AF2};
static const lv_coord_t widths[] = {3, 6, 14};
static lv_obj_t *color_btns[sizeof(palette) / sizeof(palette[0])];
static lv_obj_t *width_btn_label;
static uint8_t width_index = 1;

static void draw_segment(const lv_point_t &a, const lv_point_t &b) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = pen_color;
  dsc.width = pen_width;
  dsc.round_start = 1;
  dsc.round_end = 1;
  lv_point_t pts[2] = {a, b};
  lv_canvas_draw_line(canvas, pts, 2, &dsc);
}

static void canvas_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    has_last = false;
    return;
  }
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  lv_area_t area;
  lv_obj_get_coords(canvas, &area);
  p.x -= area.x1;
  p.y -= area.y1;
  if (p.x < 0 || p.y < 0 || p.x >= CANVAS_W_PX || p.y >= CANVAS_H) {
    has_last = false;
    return;
  }
  // Un simple appui dessine un point : segment de longueur nulle
  draw_segment(has_last ? last_pt : p, p);
  last_pt = p;
  has_last = true;
}

static void select_color(uint8_t idx) {
  pen_color = lv_color_hex(palette[idx]);
  for (uint8_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
    lv_obj_set_style_border_width(color_btns[i], i == idx ? 3 : 0, 0);
  }
}

static void color_btn_cb(lv_event_t *e) {
  select_color((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void width_btn_cb(lv_event_t *e) {
  width_index = (width_index + 1) % (sizeof(widths) / sizeof(widths[0]));
  pen_width = widths[width_index];
  lv_label_set_text_fmt(width_btn_label, "Trait %d", pen_width);
}

static void clear_btn_cb(lv_event_t *e) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label_out) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 80, 36);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  if (label_out) *label_out = l;
  return btn;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1C1C1E), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Palette : 6 pastilles sur une ligne
  for (uint8_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_set_size(b, 24, 24);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(palette[i]), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 2);
    lv_obj_add_event_cb(b, color_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_set_pos(b, 4 + i * 28, 8);
    color_btns[i] = b;
  }

  lv_obj_t *wbtn = make_btn(scr, "Trait 6", width_btn_cb, &width_btn_label);
  lv_obj_set_pos(wbtn, 4, 46);
  lv_obj_t *cbtn = make_btn(scr, "Effacer", clear_btn_cb, NULL);
  lv_obj_set_pos(cbtn, 88, 46);

  canvas_buf = (lv_color_t *)heap_caps_malloc(
      LV_CANVAS_BUF_SIZE_TRUE_COLOR(CANVAS_W_PX, CANVAS_H), MALLOC_CAP_SPIRAM);
  if (!canvas_buf) {
    lv_obj_t *err = lv_label_create(scr);
    lv_label_set_text(err, "PSRAM\nindisponible");
    lv_obj_center(err);
    return;
  }
  canvas = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W_PX, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
  lv_obj_set_pos(canvas, 0, TOOLBAR_H);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, canvas_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(canvas, canvas_event_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(canvas, canvas_event_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(canvas, canvas_event_cb, LV_EVENT_PRESS_LOST, NULL);

  select_color(0);
}

void setup() {
  Serial.begin(115200);
  Serial.println("App Dessin");
  if (!display_init()) {
    Serial.println("ERREUR : initialisation de l'ecran");
    return;
  }
  if (display_lock(-1)) {
    build_ui();
    display_unlock();
  }
  delay(100);
  display_set_brightness(255);
}

void loop() {
  delay(1000);
}
