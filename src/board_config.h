#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// Brochage Waveshare ESP32-S3-Touch-LCD-3.49 **V2**
// (repris de waveshareteam/ESP32-S3-Touch-LCD-3.49-V2, Arduino/examples/09_LVGL_V8_Test/user_config.h)
// En V1, LCD_BL <-> EXIO_INT et LCD_TE <-> LCD_RESET sont inversés : ne pas réutiliser tel quel.

#include "driver/gpio.h"

#define LCD_HOST SPI3_HOST

// I2C bus 1 : tactile AXS15231B
#define Touch_SCL_NUM (GPIO_NUM_18)
#define Touch_SDA_NUM (GPIO_NUM_17)

// I2C bus 0 : TCA9554, RTC PCF85063, IMU QMI8658
#define ESP_SCL_NUM (GPIO_NUM_48)
#define ESP_SDA_NUM (GPIO_NUM_47)

// Écran QSPI AXS15231B
#define EXAMPLE_PIN_NUM_LCD_CS     (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK   (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA0  (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1  (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2  (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3  (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_TE     (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_LCD_RST    (-1)          // reset via TCA9554 (EXIO5)
#define EXAMPLE_PIN_NUM_BK_LIGHT   (GPIO_NUM_42) // PWM, actif à l'état bas
#define EXAMPLE_PIN_NUM_EXIO_INT   (GPIO_NUM_8)

#define EXAMPLE_PIN_NUM_BAT_ADC    (GPIO_NUM_4)
#define EXAMPLE_PIN_NUM_SYS_OUT    (GPIO_NUM_16)

// Sorties de l'expandeur TCA9554
#define EXAMPLE_EXIO_PIN_TOUCH_INT (1ULL << 0)
#define EXAMPLE_EXIO_PIN_BL_EN     (1ULL << 1)
#define EXAMPLE_EXIO_PIN_IMU_INT1  (1ULL << 2)
#define EXAMPLE_EXIO_PIN_IMU_INT2  (1ULL << 3)
#define EXAMPLE_EXIO_PIN_RTC_INT   (1ULL << 4)
#define EXAMPLE_EXIO_PIN_LCD_RST   (1ULL << 5)
#define EXAMPLE_EXIO_PIN_SYS_EN    (1ULL << 6)
#define EXAMPLE_EXIO_PIN_NS_MODE   (1ULL << 7)

#define I2C_TOUCH_ADDR    0x3b
#define EXAMPLE_RTC_ADDR  0x51
#define EXAMPLE_IMU_ADDR  0x6b

#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 5

// Écran en portrait, sans rotation logicielle
#define EXAMPLE_LCD_H_RES 172
#define EXAMPLE_LCD_V_RES 640

#define LCD_NOROT_HRES     172
#define LCD_NOROT_VRES     640
#define LVGL_DMA_BUFF_LEN (LCD_NOROT_HRES * 64 * 2)
#define LVGL_SPIRAM_BUFF_LEN (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

#endif
