#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise I2C, TCA9554, écran QSPI, tactile et LVGL (tâche sur le coeur 0).
// Retourne false si un tampon PSRAM n'a pas pu être alloué.
bool display_init(void);

// Toute manipulation d'objets LVGL hors de la tâche LVGL doit être encadrée par ces deux appels.
bool display_lock(int timeout_ms);
void display_unlock(void);

// Luminosité 0 (éteint) à 255 (maximum).
void display_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif
