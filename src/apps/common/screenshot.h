#pragma once
// Capture d'écran par le port série USB, pour illustrer l'appli dans l'appstore.
//
// Le PC envoie la ligne "SCREENSHOT" ; l'appli répond :
//   "\n#SCREENSHOT <largeur> <hauteur> RGB565BE\n" + largeur*hauteur*2 octets + "\n#END\n"
// scripts/screenshot.py fait la demande, enregistre le PNG et l'ajoute à l'appstore.
//
// Usage (dans un seul .cpp par appli) :
//   screenshot_begin(lvgl_lock, lvgl_unlock);   // fonctions de verrou LVGL de l'appli
//   screenshot_poll();                          // dans loop(), si l'appli n'utilise pas Improv
//   improv_set_line_hook(screenshot_handle_line);  // sinon : Improv transmet les lignes texte
// Nécessite LV_USE_SNAPSHOT 1 dans lv_conf.h.
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

namespace screenshot_detail {
static bool (*lockFn)(int) = nullptr;
static void (*unlockFn)() = nullptr;
}

static inline void screenshot_begin(bool (*lock)(int), void (*unlock)()) {
  screenshot_detail::lockFn = lock;
  screenshot_detail::unlockFn = unlock;
}

// Rend l'écran actif dans un tampon PSRAM et l'envoie en binaire sur le port série.
static inline void screenshot_send() {
  using namespace screenshot_detail;
  if (!lockFn || !lockFn(2000)) { Serial.println("\n#SCREENSHOT-ERROR verrou"); return; }
  lv_obj_t *scr = lv_scr_act();
  uint32_t size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
  uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  lv_img_dsc_t dsc;
  bool ok = buf && lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, size) == LV_RES_OK;
  unlockFn();
  if (!ok) {
    Serial.println("\n#SCREENSHOT-ERROR capture");
    if (buf) heap_caps_free(buf);
    return;
  }
  // LV_COLOR_16_SWAP = 1 : les pixels sont déjà en RGB565 octet fort en premier
  Serial.printf("\n#SCREENSHOT %d %d %s\n", (int)dsc.header.w, (int)dsc.header.h,
                LV_COLOR_16_SWAP ? "RGB565BE" : "RGB565LE");
  uint32_t n = (uint32_t)dsc.header.w * dsc.header.h * 2, sent = 0;
  while (sent < n) {
    size_t chunk = n - sent > 4096 ? 4096 : n - sent;
    sent += Serial.write(buf + sent, chunk);
  }
  Serial.print("\n#END\n");
  Serial.flush();
  heap_caps_free(buf);
}

// Commande texte reçue sur le port série (sans le saut de ligne).
static inline void screenshot_handle_line(const char *line) {
  if (strcmp(line, "SCREENSHOT") == 0) screenshot_send();
}

// Pour les applis sans Improv : lit les lignes du port série.
static inline void screenshot_poll() {
  static char line[32];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { line[n] = 0; n = 0; screenshot_handle_line(line); continue; }
    if (n < (int)sizeof(line) - 1) line[n++] = c;
  }
}
