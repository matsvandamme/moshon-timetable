#pragma once

#include "irail.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MODE_DEPARTURES,   // "Vertrek"
    UI_MODE_ARRIVALS,     // "Aankomst"
} ui_mode_t;

// Build the screen tree (header + 5 board rows). Must be called before any
// of the update functions below. Internally locks LVGL.
esp_err_t ui_build(void);

// Switch which board is showing. Updates the header tab highlight and
// re-renders the body rows from the currently-set boards. Triggered both by
// the in-header tab tap and by external callers. Safe from any task.
void ui_set_mode(ui_mode_t mode);

// Hand the UI both boards. The currently-selected mode determines which one
// gets rendered; the other is kept for instant swapping when the user taps
// the inactive tab. Pass NULL to clear a side.
void ui_set_boards(const irail_board_t *deps, const irail_board_t *arrs);

// Update the header clock + a small wifi/refresh indicator. Call ~1 Hz.
void ui_tick_status(bool wifi_ok);

// Show / hide a full-screen overlay that obscures the board view. Used for
// "Verbinding maken..." on first boot and "Verbinding hervatten..." after a
// drop. `message` may be NULL to keep the previously-set text.
void ui_show_overlay(const char *message);
void ui_hide_overlay(void);

// Variant of the overlay used while the device is in AP-mode provisioning:
// big NMBS logo, "Setup mode" headline, two instruction lines telling the
// user which Wi-Fi to join and which URL to open, plus the build info at
// the bottom. Stays on screen until the chip is rebooted by /save.
void ui_show_provisioning(const char *ap_ssid, const char *url);

#ifdef __cplusplus
}
#endif
