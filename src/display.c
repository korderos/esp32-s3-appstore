// Portage LVGL 8 de l'écran AXS15231B, adapté de lvgl_port.c (exemple Waveshare 09_LVGL_V8_Test, V2)
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "board_config.h"
#include "display.h"
#include "i2c_bsp.h"
#include "drivers/axs15231b/esp_lcd_axs15231b.h"
#include "drivers/tca9554/esp_io_expander_tca9554.h"
#include "drivers/lcd_bl_bsp/lcd_bl_pwm_bsp.h"

#define LCD_BIT_PER_PIXEL (16)

static const char *TAG = "display";
static SemaphoreHandle_t lvgl_mux = NULL;
static SemaphoreHandle_t lvgl_flush_semap = NULL;
static uint16_t *lvgl_dma_buf = NULL;
static esp_io_expander_handle_t io_expander = NULL;

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
  {0x11, (uint8_t []){0x00}, 0, 100},
  {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t task_woken = pdFALSE;
  xSemaphoreGiveFromISR(lvgl_flush_semap, &task_woken);
  return task_woken == pdTRUE;
}

static void increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

// LVGL rend une image complète en PSRAM ; on l'envoie par bandes de 64 lignes via un tampon DMA interne.
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
  const int flush_count = LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN;
  const int band_height = LCD_NOROT_VRES / flush_count;
  const int band_pixels = LVGL_DMA_BUFF_LEN / 2;
  uint16_t *map = (uint16_t *)color_map;
  int y1 = 0;

  xSemaphoreGive(lvgl_flush_semap);
  for (int i = 0; i < flush_count; i++) {
    xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
    memcpy(lvgl_dma_buf, map, LVGL_DMA_BUFF_LEN);
    esp_lcd_panel_draw_bitmap(panel, 0, y1, LCD_NOROT_HRES, y1 + band_height, lvgl_dma_buf);
    y1 += band_height;
    map += band_pixels;
  }
  xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
  lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
  uint8_t buff[32] = {0};
  if (i2c_master_touch_write_read(disp_touch_dev_handle, read_touchpad_cmd, 11, buff, 32) != ESP_OK) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  uint16_t x = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  uint16_t y = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  if (buff[1] > 0 && buff[1] < 5) {
    if (x > EXAMPLE_LCD_V_RES) x = EXAMPLE_LCD_V_RES;
    if (y > EXAMPLE_LCD_H_RES) y = EXAMPLE_LCD_H_RES;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = y;
    data->point.y = EXAMPLE_LCD_V_RES - x;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

bool display_lock(int timeout_ms)
{
  const TickType_t ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

void display_unlock(void)
{
  xSemaphoreGive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
  uint32_t delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for (;;) {
    if (display_lock(-1)) {
      delay_ms = lv_timer_handler();
      display_unlock();
    }
    if (delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
      delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    } else if (delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
      delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}

static void backlight_off_early(void)
{
  gpio_config_t conf = {};
  conf.intr_type = GPIO_INTR_DISABLE;
  conf.mode = GPIO_MODE_OUTPUT;
  conf.pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_BK_LIGHT);
  conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
  conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&conf));
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 0));
}

static void exio_init(void)
{
  i2c_master_bus_handle_t bus = NULL;
  ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &bus));
  ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
}

static void lcd_reset(void)
{
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 0));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
}

void display_set_brightness(uint8_t level)
{
  // La PWM du rétroéclairage est inversée : rapport cyclique 0 = luminosité maximale
  setUpduty(0xff - level);
}

bool display_init(void)
{
  static lv_disp_draw_buf_t disp_buf;
  static lv_disp_drv_t disp_drv;
  static lv_indev_drv_t indev_drv;

  lvgl_flush_semap = xSemaphoreCreateBinary();

  ESP_LOGI(TAG, "I2C, TCA9554, reset LCD");
  i2c_master_Init();
  backlight_off_early();
  exio_init();

  ESP_LOGI(TAG, "Bus QSPI");
  spi_bus_config_t buscfg = {};
  buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
  buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
  buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
  buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
  buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
  buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t panel_io = NULL;
  esp_lcd_panel_handle_t panel = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 3;
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = notify_flush_ready;
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));

  axs15231b_vendor_config_t vendor_config = {};
  vendor_config.flags.use_qspi_interface = 1;
  vendor_config.init_cmds = lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
  panel_config.vendor_config = &vendor_config;

  ESP_LOGI(TAG, "Pilote AXS15231B");
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));
  lcd_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 1));

  lv_init();

  lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
  lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  if (!lvgl_dma_buf || !buffer_1 || !buffer_2) {
    ESP_LOGE(TAG, "Allocation des tampons impossible (PSRAM active ?)");
    return false;
  }
  lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 1;  // le flush par bandes suppose une image complète
  disp_drv.user_data = panel;
  lv_disp_drv_register(&disp_drv);

  esp_timer_create_args_t tick_args = {};
  tick_args.callback = &increase_lvgl_tick;
  tick_args.name = "lvgl_tick";
  esp_timer_handle_t tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  lvgl_mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", 6144, NULL, 4, NULL, 0);

  lcd_bl_pwm_bsp_init(0xff);  // rétroéclairage éteint jusqu'au premier rendu
  return true;
}
