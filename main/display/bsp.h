#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// -------- WT32-SC01 Plus pin map (ESP32-S3-WROVER, ST7796 i80, FT6336U I2C) --------
// Confirmed against several open-source projects (TFT_eSPI discussion #2319,
// RIOT-OS board file, ESPHome community thread). Touch RST is shared with LCD
// RST on this board; backlight is GPIO45 (PWM-able).
//
// If the display stays blank on first flash, suspect these in order:
//   1. PCLK polarity / timing (LCD_CMD_BITS, LCD_PARAM_BITS).
//   2. RST shared timing (touch may need extra delay after LCD reset).
//   3. Backlight not driven (GPIO45 must be HIGH for visible image).

#define BSP_LCD_PIN_DATA0   9
#define BSP_LCD_PIN_DATA1   46
#define BSP_LCD_PIN_DATA2   3
#define BSP_LCD_PIN_DATA3   8
#define BSP_LCD_PIN_DATA4   18
#define BSP_LCD_PIN_DATA5   17
#define BSP_LCD_PIN_DATA6   16
#define BSP_LCD_PIN_DATA7   15
#define BSP_LCD_PIN_PCLK    47
#define BSP_LCD_PIN_DC      0
#define BSP_LCD_PIN_RST     4
#define BSP_LCD_PIN_CS      -1
#define BSP_LCD_PIN_BL      45

#define BSP_TOUCH_I2C_NUM   0
#define BSP_TOUCH_I2C_FREQ  400000
#define BSP_TOUCH_PIN_SDA   6
#define BSP_TOUCH_PIN_SCL   5
#define BSP_TOUCH_PIN_INT   7
#define BSP_TOUCH_PIN_RST   -1

// -------- Public API --------
//
// bsp_init() does:
//   1. Initialise the i80 bus + ST7796 panel.
//   2. Initialise I2C + FT6336 touch.
//   3. Configure backlight on GPIO45 (full brightness by default).
//   4. Allocate two LVGL draw buffers (in PSRAM, line-sized) and create a
//      lv_display_t flushed via esp_lcd.
//   5. Hook up LVGL touch input device.
//   6. Spawn a FreeRTOS task that periodically calls lv_timer_handler() and
//      manages LVGL's tick base. After this returns, LVGL is fully running and
//      it is safe to create screens / widgets from the main task (LVGL itself
//      is locked via a mutex; use bsp_lvgl_lock/unlock from non-LVGL tasks).
esp_err_t bsp_init(void);

// Set backlight 0..100 %.
void bsp_set_backlight(uint8_t pct);

// Lock/unlock LVGL — required when touching widgets from a task that is NOT
// the LVGL task. Returns true if acquired, false on timeout.
bool bsp_lvgl_lock(uint32_t timeout_ms);
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
