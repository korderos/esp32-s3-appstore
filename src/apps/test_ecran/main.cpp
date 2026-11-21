#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <lvgl.h>
#include "../../display.h"

// Firmware de test : USB CDC + écran AXS15231B (LVGL 8) + tactile + PSRAM.

static lv_obj_t *uptime_label;
static lv_obj_t *touch_label;
static lv_obj_t *bright_label;
static const uint8_t brightness_steps[] = {255, 160, 80, 20};
static uint8_t brightness_index = 0;

static void printChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  Serial.println();
  Serial.println("=== ESP32-S3-Touch-LCD-3.49 V2 : test ecran ===");
  Serial.printf("Puce      : %s rev %d, %d coeurs @ %lu MHz\n",
                ESP.getChipModel(), info.revision, info.cores,
                (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Flash     : %lu Mo\n", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("PSRAM     : %lu Ko (libre %lu Ko)\n",
                (unsigned long)(ESP.getPsramSize() / 1024), (unsigned long)(ESP.getFreePsram() / 1024));
  Serial.printf("Heap libre: %lu octets\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("Reset     : raison %d\n", (int)esp_reset_reason());
  Serial.println("===============================================");
}

static void brightness_btn_cb(lv_event_t *e) {
  brightness_index = (brightness_index + 1) % sizeof(brightness_steps);
  display_set_brightness(brightness_steps[brightness_index]);
  lv_label_set_text_fmt(bright_label, "Luminosite %d", brightness_steps[brightness_index]);
}

static void screen_touch_cb(lv_event_t *e) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  lv_label_set_text_fmt(touch_label, "Touche\nx=%d y=%d", p.x, p.y);
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "ESP32-S3\nLCD 3.49 V2");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  // Bandes de couleur pour vérifier l'ordre RGB et l'inversion d'octets
  const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF};
  const char *names[] = {"Rouge", "Vert", "Bleu"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 140, 44);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors[i]), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 100 + i * 54);
    lv_obj_t *l = lv_label_create(bar);
    lv_label_set_text(l, names[i]);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
  }

  lv_obj_t *psram = lv_label_create(scr);
  lv_label_set_text_fmt(psram, "PSRAM %lu Ko", (unsigned long)(ESP.getPsramSize() / 1024));
  lv_obj_set_style_text_color(psram, lv_color_hex(0xA0C4FF), 0);
  lv_obj_align(psram, LV_ALIGN_TOP_MID, 0, 280);

  uptime_label = lv_label_create(scr);
  lv_obj_set_style_text_font(uptime_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(uptime_label, lv_color_hex(0xFFD166), 0);
  lv_label_set_text(uptime_label, "0 s");
  lv_obj_align(uptime_label, LV_ALIGN_TOP_MID, 0, 320);

  touch_label = lv_label_create(scr);
  lv_obj_set_style_text_color(touch_label, lv_color_white(), 0);
  lv_obj_set_style_text_align(touch_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(touch_label, "Touchez l'ecran");
  lv_obj_align(touch_label, LV_ALIGN_TOP_MID, 0, 390);

  lv_obj_t *btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 150, 60);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -70);
  lv_obj_add_event_cb(btn, brightness_btn_cb, LV_EVENT_CLICKED, NULL);
  bright_label = lv_label_create(btn);
  lv_label_set_text(bright_label, "Luminosite 255");
  lv_obj_center(bright_label);
}

void setup() {
  Serial.begin(115200);
  // Laisse le temps au PC d'ouvrir le port, sans bloquer si rien n'est branché
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {
    delay(10);
  }
  printChipInfo();

  if (!display_init()) {
    Serial.println("ERREUR : initialisation de l'ecran");
    return;
  }
  if (display_lock(-1)) {
    build_ui();
    display_unlock();
  }
  delay(100);  // laisse LVGL dessiner une première image avant d'allumer
  display_set_brightness(brightness_steps[brightness_index]);
  Serial.println("Ecran initialise");
}

void loop() {
  static uint32_t n = 0;
  uint32_t s = millis() / 1000;
  if (uptime_label && display_lock(100)) {
    lv_label_set_text_fmt(uptime_label, "%lu s", (unsigned long)s);
    display_unlock();
  }
  Serial.printf("[%lu s] vivant, heap %lu, PSRAM libre %lu Ko\n",
                (unsigned long)s, (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(ESP.getFreePsram() / 1024));
  if (++n % 30 == 0) {
    printChipInfo();
  }
  delay(1000);
}
