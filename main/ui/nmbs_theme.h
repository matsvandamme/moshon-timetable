#pragma once

#include "lvgl.h"

// -------- NMBS / SNCB palette --------
// Best-effort approximation of NMBS / SNCB visual identity. The official brand
// guide is not public, so these values are derived from the train livery,
// belgiantrain.be web header, and Pantone 295 C (the classic NMBS rail blue).
//
// Update these in one place to re-skin the whole UI.

#define NMBS_BLUE_DEEP     lv_color_hex(0x002F6C)  // Pantone 295 C (primary)
#define NMBS_BLUE_DARK     lv_color_hex(0x0A1F44)  // panel background
#define NMBS_BLUE_MED      lv_color_hex(0x1F4E96)  // secondary, hovered rows
#define NMBS_BLUE_SOFT     lv_color_hex(0x2B7BB9)  // lighter accent / dividers
#define NMBS_YELLOW        lv_color_hex(0xFCD500)  // accent (departure marker, B-mark)
#define NMBS_RED           lv_color_hex(0xD52A33)  // delay / cancellation
#define NMBS_WHITE         lv_color_hex(0xFFFFFF)
#define NMBS_OFFWHITE      lv_color_hex(0xF4F4F4)
#define NMBS_GREY          lv_color_hex(0x6E7882)
#define NMBS_BLACK         lv_color_hex(0x111418)
