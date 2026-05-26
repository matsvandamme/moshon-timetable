#pragma once

#include "sdkconfig.h"

// -------- Project-wide constants --------
#define APP_NAME           "moshon-timetable"
#define APP_VERSION        "0.2.0"
#define APP_DEVELOPER      "VMAT"
#define APP_CONTACT_EMAIL  "matthieu@vmat-contracting.be"
#define APP_HOMEPAGE       "https://github.com/matsvandamme/moshon-timetable"
#define APP_HOMEPAGE_SHORT "github.com/matsvandamme/moshon-timetable"

// User-Agent string required by iRail TOS:
//   "<application name>/<version> (<website>; <email>)"
#define APP_USER_AGENT \
    APP_NAME "/" APP_VERSION " (" APP_HOMEPAGE "; " APP_CONTACT_EMAIL ")"

// -------- Station --------
// The active station is selected via `idf.py menuconfig` -> "Moshon
// Timetable" -> "Active NMBS station". The selection is read at runtime
// via station_get_active() from stations.h.
//
// To change the station: run `idf.py menuconfig`, pick a different entry
// in the "Active NMBS station" menu, save, and rebuild. To add a new
// station: add an entry to firmware/main/stations.{h,c} and a matching
// `config STATION_*` to Kconfig.projbuild.

// -------- Display geometry (WT32-SC01 Plus, ST7796, landscape) --------
#define LCD_H_RES          480
#define LCD_V_RES          320
#define LCD_BITS_PER_PIXEL 16
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)  // 20 MHz — safe; raise once stable

// -------- Refresh policy --------
// Sourced from Kconfig (default 45 s, range 15-600). See Kconfig.projbuild.
#define REFRESH_PERIOD_MS  (CONFIG_REFRESH_PERIOD_SECONDS * 1000)

// -------- Logging tags --------
#define TAG_APP            "app"
#define TAG_BSP            "bsp"
#define TAG_WIFI           "wifi"
#define TAG_IRAIL          "irail"
#define TAG_UI             "ui"
