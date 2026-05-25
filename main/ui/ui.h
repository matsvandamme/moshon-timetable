#pragma once

#include "irail.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the initial UI tree (header, two columns, footer). Must be called
// from within bsp_lvgl_lock() / unlock() (or before the LVGL task is started).
esp_err_t ui_build(void);

// Update both boards. Safe to call from any task — the function locks LVGL
// internally. Passing NULL keeps that side's current display.
void ui_update_boards(const irail_board_t *deps, const irail_board_t *arrs);

// Update the footer status (wifi state, last refresh time, etc.).
void ui_update_status(bool wifi_ok, time_t last_refresh);

#ifdef __cplusplus
}
#endif
