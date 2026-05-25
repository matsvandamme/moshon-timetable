#pragma once

// -------- Project-wide constants --------
#define APP_NAME           "moshon-timetable"
#define APP_VERSION        "0.1.0"
#define APP_CONTACT_EMAIL  "matthieu@vmat-contracting.be"
#define APP_HOMEPAGE       "https://github.com/matsvandamme/moshon-timetable"

// User-Agent string required by iRail TOS:
//   "<application name>/<version> (<website>; <email>)"
#define APP_USER_AGENT \
    APP_NAME "/" APP_VERSION " (" APP_HOMEPAGE "; " APP_CONTACT_EMAIL ")"

// -------- Station --------
// Aalter on the Brussels–Ostend line. URI id: 008891140.
#define STATION_NAME       "Aalter"
#define STATION_ID         "BE.NMBS.008891140"

// -------- Display geometry (WT32-SC01 Plus, ST7796, landscape) --------
#define LCD_H_RES          480
#define LCD_V_RES          320
#define LCD_BITS_PER_PIXEL 16
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)  // 20 MHz — safe; raise once stable

// -------- Refresh policy --------
// iRail asks clients to be polite. 30s is a reasonable lower bound for a
// passenger info display; 60s is fine for arrivals/departures that don't
// change second-by-second.
#define REFRESH_PERIOD_MS  (45 * 1000)

// -------- Logging tags --------
#define TAG_APP            "app"
#define TAG_BSP            "bsp"
#define TAG_WIFI           "wifi"
#define TAG_IRAIL          "irail"
#define TAG_UI             "ui"
