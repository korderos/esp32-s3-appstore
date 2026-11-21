#ifndef LVGL_PORT_H
#define LVGL_PORT_H



#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
void lvgl_port_init(void);
bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);
bool lvgl_get_flip(void);
void lvgl_set_flip(bool flip);


#ifdef __cplusplus
}
#endif



#endif










