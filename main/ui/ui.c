// NMBS-style station departures/arrivals board for the WT32-SC01 Plus.
// Layout mirrors the in-station NMBS Vertrek/Aankomst displays:
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ HH:MM   Vertrek                                  [ B ]   │  header (40 px)
//   ├────────┬───────────────────────────────────────┬─────────┤
//   │ time   │ destination                            │ plat   │
//   │ +d'    │ vehicle  (e.g. "IC 1538")              │ │ type │
//   │ new    │                                        │ │      │
//   ├────────┴───────────────────────────────────────┴─────────┤
//   │ ... 5 rows ...                                            │
//
// Display alternates between Vertrek and Aankomst every BOARD_TOGGLE_SECONDS.

#include "ui.h"
#include "nmbs_theme.h"
#include "nmbs_logo.h"
#include "bsp.h"
#include "app_config.h"
#include "stations.h"
#include "cfg.h"
#include "i18n.h"
#include "esp_system.h"
#include "libs/qrcode/lv_qrcode.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// -------- Geometry --------
#define HEADER_H        40
#define ROW_H           54
#define N_ROWS          5
#define BODY_TOP        HEADER_H
#define BODY_H          (N_ROWS * ROW_H)

// Per-row columns (within a 480-px-wide row):
#define COL_TIME_X      6
#define COL_TIME_W      82
#define COL_SEP1_X      92
#define COL_DEST_X      100
#define COL_DEST_W      252
#define COL_SEP2_X      356
#define COL_PLAT_X      364
#define COL_PLAT_W      44
#define COL_TYPE_X      414
#define COL_TYPE_W      62

// -------- Slot indices used inside each row's user_data --------
enum {
    SLOT_TIME = 0,
    SLOT_DELAY_BOX,
    SLOT_DELAY_LBL,
    SLOT_NEW_TIME,
    SLOT_DEST,
    SLOT_VEHICLE,
    SLOT_PLAT,
    SLOT_TYPE,
    SLOT_NUMBER,    // train number (e.g. "2204") shown under the type badge
    SLOT_COUNT,
};

// -------- Per-row "via" page-flip state --------
// Each row's via label snaps between "pages". A page packs as many
// comma-separated stops as fit in COL_DEST_W (no stop is ever cut in half).
// The first page is prefixed with "via "; subsequent pages aren't.
//
// All rows are SYNCHRONISED via a single global tick counter — every row
// updates at the same instant via the same lv_timer. Each row independently
// wraps when its own pages run out.
#define MARQUEE_INTERVAL_MS  1800
#define MAX_PAGES_PER_ROW    8

typedef struct {
    char full_text[IRAIL_VIA_LEN];   // copy of e->via
    int  num_stops;                  // number of stops parsed out
    int  num_pages;                  // how many distinct pages were precomputed
    int  page_starts[MAX_PAGES_PER_ROW];  // stop index where each page begins
} via_state_t;

static int s_global_page_tick = 0;

// -------- Widget handles --------
static lv_obj_t *s_screen      = NULL;
// Wall clock as three separate labels so the colon can blink without
// shifting the digit positions (Montserrat is proportional, so swapping
// ':' for ' ' would nudge the minutes left/right every second).
static lv_obj_t *s_header_hh   = NULL;
static lv_obj_t *s_header_sep  = NULL;
static lv_obj_t *s_header_mm   = NULL;
static lv_obj_t *s_header_mode = NULL;   // shows "Vertrek" OR "Aankomst" — single title
static lv_obj_t *s_header_wifi = NULL;
static lv_obj_t *s_logo        = NULL;   // NMBS B-in-oval; clickable to toggle mode
// Small traffic-light dot indicating data freshness. Sits in the header
// just left of the Wi-Fi indicator. Driven by ui_tick_freshness().
static lv_obj_t *s_freshness_dot = NULL;
// Weather chip in the header (small "12°" / "-3°" label). Hidden until
// the user enters coordinates in the soft settings page.
static lv_obj_t *s_weather_label = NULL;
// Thin red alert bar that appears below the header when iRail reports a
// service disruption. Hidden by default.
static lv_obj_t *s_alert_bar  = NULL;
static lv_obj_t *s_alert_text = NULL;
// Wi-Fi status overlay (full-screen). Visible by default — the splash is
// the FIRST thing the user sees at boot, before app_main even sets a message.
static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_overlay_msg   = NULL;
static lv_obj_t *s_overlay_sub1  = NULL;   // secondary line (e.g. AP SSID)
static lv_obj_t *s_overlay_sub2  = NULL;   // secondary line (e.g. URL)
static lv_obj_t *s_overlay_info  = NULL;   // bottom build-info panel
static bool      s_overlay_visible = true;  // matches initial state set in ui_build
// Body container holding the 5 timetable rows. Tracked globally so the
// overlay show/hide path can flip its HIDDEN flag atomically in the same
// LVGL lock — this guarantees the timetable is NEVER on-screen while the
// overlay is up, and that a transient empty body can never flash between
// frames.
static lv_obj_t *s_body          = NULL;
static lv_obj_t *s_rows[N_ROWS] = {0};
static via_state_t s_via_state[N_ROWS] = {0};
static ui_mode_t s_mode = UI_MODE_DEPARTURES;

// Boards held by ui so a tab tap can swap the body instantly without
// waiting for the next iRail fetch.
static const irail_board_t *s_dep_board = NULL;
static const irail_board_t *s_arr_board = NULL;

static void render_entry(int row_idx, lv_obj_t *row, const irail_entry_t *e);
static void clear_row(int row_idx, lv_obj_t *row);

// Count comma-separated stops in "via X, Y, Z" — returns 0 if not the
// expected format (will then just show the raw string as-is).
static int count_via_stops(const char *full)
{
    if (!full || !full[0]) return 0;
    const char *p = full;
    if (strncmp(p, "via ", 4) == 0) p += 4;
    if (!*p) return 0;
    int count = 1;
    const char *q = p;
    while ((q = strstr(q, ", ")) != NULL) {
        count++;
        q += 2;
    }
    return count;
}

// Return a pointer to the n-th comma-separated stop in `full` and write
// its length (excluding the comma separator) into *out_len. Returns NULL
// if n is past the last stop.
static const char *get_nth_stop_ptr(const char *full, int n, size_t *out_len)
{
    *out_len = 0;
    if (!full || !full[0]) return NULL;
    const char *p = full;
    if (strncmp(p, "via ", 4) == 0) p += 4;
    int idx = 0;
    while (*p && idx < n) {
        const char *c = strstr(p, ", ");
        if (!c) return NULL;
        p = c + 2;
        idx++;
    }
    if (!*p) return NULL;
    const char *end = strstr(p, ", ");
    *out_len = end ? (size_t)(end - p) : strlen(p);
    return p;
}

// Build a page of stops starting at `start_stop`, packing as many as fit in
// COL_DEST_W when rendered in lv_font_montserrat_14. The first page (start
// == 0) is prefixed with "via "; later pages aren't. Always includes at
// least one stop even if it overflows alone, so a single very-long station
// name still gets shown. Returns the index of the FIRST stop NOT included
// (i.e. where the next page should start).
//
// We measure against (COL_DEST_W - VIA_PACK_MARGIN). The extra slack absorbs
// LVGL's internal label padding and one-pixel sub-pixel rounding, so a stop
// that "just fits" mathematically never gets truncated to "Kwat..." on screen.
#define VIA_PACK_MARGIN  12
static int build_via_page(const char *full, int start_stop,
                          char *out, size_t out_len)
{
    if (!out || out_len < 8) return start_stop;
    out[0] = '\0';

    if (start_stop == 0) {
        // Prefix
        if (out_len < 5) return start_stop;
        memcpy(out, "via ", 4);
        out[4] = '\0';
    }

    int idx = start_stop;
    while (1) {
        size_t stop_len = 0;
        const char *stop = get_nth_stop_ptr(full, idx, &stop_len);
        if (!stop) break;

        // Build a trial string: existing + (", " separator if not the first
        // stop on this page) + stop name.
        char trial[IRAIL_VIA_LEN * 2];
        const bool first_on_page = (idx == start_stop);
        if (first_on_page && start_stop > 0) {
            // Subsequent page: first stop has no leading separator
            snprintf(trial, sizeof(trial), "%.*s", (int)stop_len, stop);
        } else if (first_on_page) {
            // First page first stop: just append after "via "
            snprintf(trial, sizeof(trial), "%s%.*s", out, (int)stop_len, stop);
        } else {
            // Mid-page: comma-separate
            snprintf(trial, sizeof(trial), "%s, %.*s", out, (int)stop_len, stop);
        }

        // Measure width in our 14pt font.
        lv_point_t sz;
        lv_text_get_size(&sz, trial, &lv_font_montserrat_14,
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        // Always accept the first stop on a page, even if it's too long
        // alone — otherwise we'd never advance. Otherwise stop when adding
        // this one would push past the column width.
        if (sz.x > (COL_DEST_W - VIA_PACK_MARGIN) && !first_on_page) {
            break;
        }

        strncpy(out, trial, out_len - 1);
        out[out_len - 1] = '\0';
        idx++;
    }

    return idx;
}

static void apply_header_title_locked(void)
{
    if (!s_header_mode) return;
    lv_label_set_text(s_header_mode,
                      i18n_text(s_mode == UI_MODE_DEPARTURES
                                ? TR_DEPARTURES : TR_ARRIVALS));
}

// 1 Hz LVGL timer that ticks the wall clock + blinks the colon. Lives
// inside the LVGL task so its cadence is decoupled from main_loop_task —
// previously the clock froze for several seconds whenever the network
// fetch was busy doing TLS handshakes against iRail. The colon now blinks
// reliably regardless of what the network is up to.
static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    static bool blink_on = false;
    blink_on = !blink_on;

    if (!s_header_hh || !s_header_mm || !s_header_sep) return;
    // LVGL timers run inside the LVGL task which already holds the lock;
    // no extra bsp_lvgl_lock() needed here.

    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
    lv_label_set_text(s_header_hh, hh);
    lv_label_set_text(s_header_mm, mm);
    lv_obj_set_style_text_color(s_header_sep,
        blink_on ? NMBS_WHITE : NMBS_BLUE_DEEP, 0);
}

static void apply_render_locked(void)
{
    const irail_board_t *b = (s_mode == UI_MODE_DEPARTURES) ? s_dep_board : s_arr_board;
    for (int i = 0; i < N_ROWS; i++) {
        if (b && i < (int)b->count) render_entry(i, s_rows[i], &b->entries[i]);
        else                          clear_row(i, s_rows[i]);
    }
}

// Periodic timer (~MARQUEE_INTERVAL_MS): advances the SHARED page index
// across every row in lockstep. All rows show their page-0 ("via …") at
// tick 0, then the index climbs. A row that has fewer pages than the index
// goes blank for the remainder of the cycle — when the longest row reaches
// its last page the tick wraps and everyone snaps back to "via …" together.
//
// Rows without any via (num_pages == 0) are untouched here — render_entry
// keeps the vehicle shortname in that slot as a fallback.
static void marquee_tick(lv_timer_t *t)
{
    (void)t;
    if (!bsp_lvgl_lock(50)) return;
    // Skip while the Wi-Fi overlay is up — no point ticking labels that are
    // covered, and the partial redraws can otherwise leak through if the
    // overlay isn't fully repainted in time.
    if (s_overlay_visible) {
        bsp_lvgl_unlock();
        return;
    }

    // Cycle length = longest row's page count, so all rows realign at 0.
    int max_pages = 1;
    for (int i = 0; i < N_ROWS; i++) {
        if (s_via_state[i].num_pages > max_pages) {
            max_pages = s_via_state[i].num_pages;
        }
    }
    s_global_page_tick = (s_global_page_tick + 1) % max_pages;

    for (int i = 0; i < N_ROWS; i++) {
        via_state_t *s = &s_via_state[i];
        if (s->num_pages <= 0) continue;            // no via at all: fallback text
        lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(s_rows[i]);
        if (!slots || !slots[SLOT_VEHICLE]) continue;

        if (s_global_page_tick < s->num_pages) {
            int start = s->page_starts[s_global_page_tick];
            char display[IRAIL_VIA_LEN * 2];
            build_via_page(s->full_text, start, display, sizeof(display));
            lv_label_set_text(slots[SLOT_VEHICLE], display);
        } else {
            // This row has run out of pages this cycle — blank until wrap.
            lv_label_set_text(slots[SLOT_VEHICLE], "");
        }
    }
    bsp_lvgl_unlock();
}

// Precompute the stop index that each page begins at, given the row's
// full via text. Returns the number of pages (1..MAX_PAGES_PER_ROW).
static int precompute_pages(const char *full, int num_stops,
                            int *page_starts, int max_pages)
{
    if (num_stops <= 0 || max_pages < 1) return 0;
    int pages = 0;
    int start = 0;
    while (pages < max_pages) {
        page_starts[pages++] = start;
        char tmp[IRAIL_VIA_LEN * 2];
        int next = build_via_page(full, start, tmp, sizeof(tmp));
        if (next >= num_stops) break;            // last page
        if (next == start) break;                // safety: didn't advance
        start = next;
    }
    return pages;
}

// Force a full repaint of the active screen. Call after any major
// transition (overlay shown/hidden, mode toggle) so the partial-render
// path can't leave stale pixels from the previous frame behind. Caller
// MUST already hold the LVGL lock.
//
// Just ONE lv_refr_now: invoking it twice back-to-back was overlapping
// two DMA flushes through the same i80 buffer slot, occasionally
// producing red/green colour bleed into widgets that hadn't changed.
static void ui_full_redraw_locked(void)
{
    if (!s_screen) return;
    lv_obj_invalidate(s_screen);
    lv_refr_now(NULL);
}

// Tap the NMBS B-logo to toggle Vertrek <-> Aankomst. Runs inside the LVGL
// task which already holds the lock.
static void on_logo_tap(lv_event_t *e)
{
    (void)e;
    s_mode = (s_mode == UI_MODE_DEPARTURES) ? UI_MODE_ARRIVALS : UI_MODE_DEPARTURES;
    apply_header_title_locked();
    apply_render_locked();
    ui_full_redraw_locked();   // wipe any Vertrek leftover before Aankomst lands
}

// 3-second hold anywhere on the screen wipes Wi-Fi credentials AND the
// saved station name from NVS, then reboots into AP-mode provisioning.
// The hold time is set in bsp.c via lv_indev_set_long_press_time.
static void on_long_press_reset(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG_UI, "Long-press detected: erasing all NVS config + restarting");
    cfg_erase_all();
    // Visual cue before the chip resets — the overlay paints almost
    // immediately even if esp_restart is queued right after.
    ui_show_overlay(i18n_text(TR_RESET));
    esp_restart();
}

// -------- Styles --------
static lv_style_t st_screen_bg;
static lv_style_t st_overlay_bg;        // pure black, fully opaque
static lv_style_t st_header;
static lv_style_t st_logo_box;
static lv_style_t st_text_clock;
static lv_style_t st_text_mode;
static lv_style_t st_row;
static lv_style_t st_row_alt;
static lv_style_t st_yellow_sep;
static lv_style_t st_text_time;
static lv_style_t st_text_new_time;
static lv_style_t st_delay_box;
static lv_style_t st_text_delay;
static lv_style_t st_text_dest;
static lv_style_t st_text_dest_cancel;
static lv_style_t st_text_vehicle;
static lv_style_t st_plat_box;
static lv_style_t st_text_plat;
static lv_style_t st_text_type;
static lv_style_t st_text_number;       // small white digits under the type
static lv_style_t st_wifi_indicator;

static void styles_init(void)
{
    lv_style_init(&st_screen_bg);
    lv_style_set_bg_color(&st_screen_bg, NMBS_BLUE_DARK);
    lv_style_set_bg_opa(&st_screen_bg, LV_OPA_COVER);
    lv_style_set_text_color(&st_screen_bg, NMBS_WHITE);
    lv_style_set_pad_all(&st_screen_bg, 0);
    lv_style_set_border_width(&st_screen_bg, 0);
    lv_style_set_radius(&st_screen_bg, 0);

    // Overlay bg: pure black, fully opaque, no border/pad/radius. Distinct
    // from the body's navy bg so any bleed-through is obvious AND the
    // overlay can't be mistaken for the boards behind it.
    lv_style_init(&st_overlay_bg);
    lv_style_set_bg_color(&st_overlay_bg, lv_color_hex(0x000000));
    lv_style_set_bg_opa(&st_overlay_bg, LV_OPA_COVER);
    lv_style_set_text_color(&st_overlay_bg, NMBS_WHITE);
    lv_style_set_pad_all(&st_overlay_bg, 0);
    lv_style_set_border_width(&st_overlay_bg, 0);
    lv_style_set_radius(&st_overlay_bg, 0);

    // Header — slightly darker / saturated band sitting above the body.
    lv_style_init(&st_header);
    lv_style_set_bg_color(&st_header, NMBS_BLUE_DEEP);
    lv_style_set_bg_opa(&st_header, LV_OPA_COVER);
    lv_style_set_border_width(&st_header, 0);
    lv_style_set_radius(&st_header, 0);
    lv_style_set_pad_all(&st_header, 0);

    // NMBS B-in-oval logo (stylized): yellow horizontal pill, navy outline,
    // bold dark navy B inside. LV_RADIUS_CIRCLE on a 60x32 box renders as a
    // horizontal stadium which reads as oval at this size.
    lv_style_init(&st_logo_box);
    lv_style_set_bg_color(&st_logo_box, NMBS_YELLOW);
    lv_style_set_bg_opa(&st_logo_box, LV_OPA_COVER);
    lv_style_set_text_color(&st_logo_box, NMBS_BLUE_DEEP);
    lv_style_set_text_font(&st_logo_box, &lv_font_montserrat_24);
    lv_style_set_text_align(&st_logo_box, LV_TEXT_ALIGN_CENTER);
    lv_style_set_radius(&st_logo_box, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&st_logo_box, 0);
    lv_style_set_border_color(&st_logo_box, NMBS_BLUE_DEEP);
    lv_style_set_border_width(&st_logo_box, 2);
    lv_style_set_border_opa(&st_logo_box, LV_OPA_COVER);

    lv_style_init(&st_text_clock);
    lv_style_set_text_color(&st_text_clock, NMBS_WHITE);
    lv_style_set_text_font(&st_text_clock, &lv_font_montserrat_24);

    lv_style_init(&st_text_mode);
    lv_style_set_text_color(&st_text_mode, NMBS_WHITE);
    lv_style_set_text_font(&st_text_mode, &lv_font_montserrat_24);

    // Single bg color across all rows — alt-row tinting is dropped so the
    // via marquee label can use the same solid bg (NMBS_BLUE_DARK), which
    // eliminates the ghost-pixel flicker when scrolling text.
    lv_style_init(&st_row);
    lv_style_set_bg_color(&st_row, NMBS_BLUE_DARK);
    lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_pad_all(&st_row, 0);
    lv_style_set_radius(&st_row, 0);

    lv_style_init(&st_row_alt);   // kept for ABI but no longer applied
    lv_style_set_bg_opa(&st_row_alt, LV_OPA_TRANSP);

    // Vertical golden separator (2 px wide).
    lv_style_init(&st_yellow_sep);
    lv_style_set_bg_color(&st_yellow_sep, NMBS_YELLOW);
    lv_style_set_bg_opa(&st_yellow_sep, LV_OPA_COVER);
    lv_style_set_border_width(&st_yellow_sep, 0);
    lv_style_set_radius(&st_yellow_sep, 0);

    // Scheduled time — big white.
    lv_style_init(&st_text_time);
    lv_style_set_text_color(&st_text_time, NMBS_WHITE);
    lv_style_set_text_font(&st_text_time, &lv_font_montserrat_20);

    // Revised time after delay — slightly smaller, still white.
    lv_style_init(&st_text_new_time);
    lv_style_set_text_color(&st_text_new_time, NMBS_WHITE);
    lv_style_set_text_font(&st_text_new_time, &lv_font_montserrat_14);

    // Red badge for "+5'" delay / cancellation tag.
    lv_style_init(&st_delay_box);
    lv_style_set_bg_color(&st_delay_box, NMBS_RED);
    lv_style_set_bg_opa(&st_delay_box, LV_OPA_COVER);
    lv_style_set_border_width(&st_delay_box, 0);
    lv_style_set_radius(&st_delay_box, 2);
    lv_style_set_pad_left(&st_delay_box, 4);
    lv_style_set_pad_right(&st_delay_box, 4);
    lv_style_set_pad_top(&st_delay_box, 0);
    lv_style_set_pad_bottom(&st_delay_box, 0);

    lv_style_init(&st_text_delay);
    lv_style_set_text_color(&st_text_delay, NMBS_WHITE);
    lv_style_set_text_font(&st_text_delay, &lv_font_montserrat_14);

    // Destination — bold yellow, primary attention-grabber.
    lv_style_init(&st_text_dest);
    lv_style_set_text_color(&st_text_dest, NMBS_YELLOW);
    lv_style_set_text_font(&st_text_dest, &lv_font_montserrat_20);

    // Destination when canceled — grey strikethrough.
    lv_style_init(&st_text_dest_cancel);
    lv_style_set_text_color(&st_text_dest_cancel, NMBS_GREY);
    lv_style_set_text_decor(&st_text_dest_cancel, LV_TEXT_DECOR_STRIKETHROUGH);

    // Vehicle id under the destination — smaller white.
    lv_style_init(&st_text_vehicle);
    lv_style_set_text_color(&st_text_vehicle, NMBS_OFFWHITE);
    lv_style_set_text_font(&st_text_vehicle, &lv_font_montserrat_14);

    // Platform badge: transparent navy interior with a thin white rounded
    // outline, holding a white number. Matches the actual in-station NMBS
    // board (the badge is NOT filled yellow).
    lv_style_init(&st_plat_box);
    lv_style_set_bg_opa(&st_plat_box, LV_OPA_TRANSP);
    lv_style_set_border_color(&st_plat_box, NMBS_WHITE);
    lv_style_set_border_width(&st_plat_box, 2);
    lv_style_set_border_opa(&st_plat_box, LV_OPA_COVER);
    lv_style_set_radius(&st_plat_box, 6);
    lv_style_set_pad_all(&st_plat_box, 0);

    lv_style_init(&st_text_plat);
    lv_style_set_text_color(&st_text_plat, NMBS_WHITE);
    lv_style_set_text_font(&st_text_plat, &lv_font_montserrat_24);
    lv_style_set_text_align(&st_text_plat, LV_TEXT_ALIGN_CENTER);

    // Train type / class (IC, S8, P, L, ...) — big white on the right.
    lv_style_init(&st_text_type);
    lv_style_set_text_color(&st_text_type, NMBS_WHITE);
    lv_style_set_text_font(&st_text_type, &lv_font_montserrat_20);
    lv_style_set_text_align(&st_text_type, LV_TEXT_ALIGN_CENTER);

    // Train number — small off-white digits centered under the class.
    lv_style_init(&st_text_number);
    lv_style_set_text_color(&st_text_number, NMBS_OFFWHITE);
    lv_style_set_text_font(&st_text_number, &lv_font_montserrat_14);
    lv_style_set_text_align(&st_text_number, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&st_wifi_indicator);
    lv_style_set_text_color(&st_wifi_indicator, NMBS_OFFWHITE);
    lv_style_set_text_font(&st_wifi_indicator, &lv_font_montserrat_14);
}

// -------- Tiny helpers --------

static lv_obj_t *plain_obj(lv_obj_t *parent, lv_style_t *style,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    if (style) lv_obj_add_style(o, style, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, lv_style_t *style, const char *txt,
                       int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    if (style) lv_obj_add_style(l, style, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void format_hhmm(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// -------- Row construction --------

static lv_obj_t *build_row(lv_obj_t *parent, int idx)
{
    lv_obj_t *row = plain_obj(parent, &st_row, 0, idx * ROW_H, LCD_H_RES, ROW_H);
    // No alt-row tinting — see styles_init() comment.

    // Time column
    lv_obj_t *t_time   = label(row, &st_text_time, "--:--", COL_TIME_X, 4);
    lv_obj_t *t_delay_box = plain_obj(row, &st_delay_box,
                                      COL_TIME_X, 30, 28, 16);
    lv_obj_add_flag(t_delay_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t_delay  = label(t_delay_box, &st_text_delay, "", 0, 0);
    lv_obj_center(t_delay);
    lv_obj_t *t_newt   = label(row, &st_text_new_time, "", COL_TIME_X + 30, 32);

    // No vertical separator lines — the real NMBS board uses whitespace
    // between columns instead of explicit dividers.

    // Destination column. CLIP (not DOT) — render_entry auto-shrinks the
    // font (20 -> 16 -> 14) until the name fits in COL_DEST_W, so we never
    // need ellipsis truncation for the yellow destination.
    lv_obj_t *t_dest    = label(row, &st_text_dest,    "...", COL_DEST_X,  6);
    lv_label_set_long_mode(t_dest,    LV_LABEL_LONG_CLIP);
    lv_obj_set_width(t_dest,    COL_DEST_W);
    // Via subtitle: just a static label. The page-flip timer above swaps
    // its text every few seconds to step through stops. No LVGL marquee.
    // CLIP (not DOT) on long_mode: build_via_page already packs stops to fit
    // with a safety margin; CLIP guarantees no station name ever shows as
    // "Kwat..." even if measurement and rendering disagree by a pixel.
    lv_obj_t *t_vehicle = label(row, &st_text_vehicle, "",    COL_DEST_X, 32);
    lv_label_set_long_mode(t_vehicle, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(t_vehicle, COL_DEST_W);

    // Platform yellow badge
    lv_obj_t *t_plat_box = plain_obj(row, &st_plat_box,
                                     COL_PLAT_X, (ROW_H - 38) / 2, COL_PLAT_W, 38);
    lv_obj_t *t_plat = label(t_plat_box, &st_text_plat, "?", 0, 0);
    lv_obj_set_width(t_plat, COL_PLAT_W);
    lv_obj_set_style_text_align(t_plat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(t_plat);

    // Train type / class — stacked above the train number, mirroring the
    // way actual in-station NMBS boards present "IC" big with "2204" small
    // underneath it.
    lv_obj_t *t_type = label(row, &st_text_type, "", COL_TYPE_X, 4);
    lv_obj_set_width(t_type, COL_TYPE_W);
    lv_obj_set_style_text_align(t_type, LV_TEXT_ALIGN_CENTER, 0);

    // Train number (e.g. "2204"). Small white digits, centered in the same
    // column directly below the type. Empty when iRail doesn't expose a
    // parseable number.
    lv_obj_t *t_number = label(row, &st_text_number, "", COL_TYPE_X, 32);
    lv_obj_set_width(t_number, COL_TYPE_W);
    lv_obj_set_style_text_align(t_number, LV_TEXT_ALIGN_CENTER, 0);

    // Stash refs for fast updates
    lv_obj_t **slots = lv_malloc_zeroed(sizeof(lv_obj_t *) * SLOT_COUNT);
    slots[SLOT_TIME]      = t_time;
    slots[SLOT_DELAY_BOX] = t_delay_box;
    slots[SLOT_DELAY_LBL] = t_delay;
    slots[SLOT_NEW_TIME]  = t_newt;
    slots[SLOT_DEST]      = t_dest;
    slots[SLOT_VEHICLE]   = t_vehicle;
    slots[SLOT_PLAT]      = t_plat;
    slots[SLOT_TYPE]      = t_type;
    slots[SLOT_NUMBER]    = t_number;
    lv_obj_set_user_data(row, slots);
    return row;
}

// -------- Row rendering --------

static void render_entry(int row_idx, lv_obj_t *row, const irail_entry_t *e)
{
    lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(row);
    if (!slots) return;

    char hhmm[8];
    format_hhmm(e->scheduled, hhmm, sizeof(hhmm));
    lv_label_set_text(slots[SLOT_TIME], hhmm);

    if (e->canceled) {
        lv_obj_clear_flag(slots[SLOT_DELAY_BOX], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(slots[SLOT_DELAY_LBL], i18n_text(TR_CANCELLED_ABBR));
        lv_label_set_text(slots[SLOT_NEW_TIME], "");
        lv_obj_add_style(slots[SLOT_DEST], &st_text_dest_cancel, 0);
    } else if (e->delay_seconds >= 60) {
        lv_obj_clear_flag(slots[SLOT_DELAY_BOX], LV_OBJ_FLAG_HIDDEN);
        // Clamp displayed delay to 999 min so the badge stays compact and
        // the snprintf can't trip -Werror=format-truncation.
        unsigned mins = (unsigned)(e->delay_seconds / 60);
        if (mins > 999) mins = 999;
        char db[16];
        snprintf(db, sizeof(db), "+%u'", mins);
        lv_label_set_text(slots[SLOT_DELAY_LBL], db);
        time_t newt = e->scheduled + e->delay_seconds;
        char newbuf[8];
        format_hhmm(newt, newbuf, sizeof(newbuf));
        lv_label_set_text(slots[SLOT_NEW_TIME], newbuf);
        lv_obj_remove_style(slots[SLOT_DEST], &st_text_dest_cancel, 0);
    } else {
        lv_obj_add_flag(slots[SLOT_DELAY_BOX], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(slots[SLOT_NEW_TIME], "");
        lv_obj_remove_style(slots[SLOT_DEST], &st_text_dest_cancel, 0);
    }

    // Auto-shrink the destination font (20 -> 16 -> 14) until the name fits
    // in COL_DEST_W. The user wants the yellow name to ALWAYS be readable on
    // a single line — even Brussels-National-Airport or other long stations.
    // Leave a small margin for sub-pixel rounding.
    const lv_font_t *dest_font = &lv_font_montserrat_20;
    {
        lv_point_t sz;
        lv_text_get_size(&sz, e->other_station, &lv_font_montserrat_20,
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > COL_DEST_W - 4) {
            lv_text_get_size(&sz, e->other_station, &lv_font_montserrat_16,
                             0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            dest_font = (sz.x > COL_DEST_W - 4) ? &lv_font_montserrat_14
                                                : &lv_font_montserrat_16;
        }
    }
    lv_obj_set_style_text_font(slots[SLOT_DEST], dest_font, 0);
    lv_label_set_text(slots[SLOT_DEST], e->other_station);
    // Initialise the via page-flip state for this row. If there's no via,
    // show the vehicle shortname instead (e.g. "IC 2804") as a fallback.
    via_state_t *vs = &s_via_state[row_idx];
    if (e->via[0]) {
        strncpy(vs->full_text, e->via, IRAIL_VIA_LEN - 1);
        vs->full_text[IRAIL_VIA_LEN - 1] = '\0';
        vs->num_stops = count_via_stops(vs->full_text);
        vs->num_pages = precompute_pages(vs->full_text, vs->num_stops,
                                         vs->page_starts, MAX_PAGES_PER_ROW);
        char page[IRAIL_VIA_LEN * 2];
        if (vs->num_pages > 0) {
            // Match the synchronised cycle: if the current global tick is
            // past this row's last page, blank for the rest of the cycle so
            // a freshly-rendered row doesn't briefly desync the column.
            if (s_global_page_tick < vs->num_pages) {
                build_via_page(vs->full_text,
                               vs->page_starts[s_global_page_tick],
                               page, sizeof(page));
            } else {
                page[0] = '\0';
            }
        } else {
            // Unparseable — show the raw string as-is.
            strncpy(page, vs->full_text, sizeof(page) - 1);
            page[sizeof(page) - 1] = '\0';
        }
        lv_label_set_text(slots[SLOT_VEHICLE], page);
    } else {
        vs->full_text[0] = '\0';
        vs->num_stops    = 0;
        vs->num_pages    = 0;
        lv_label_set_text(slots[SLOT_VEHICLE], e->vehicle);
    }
    lv_label_set_text(slots[SLOT_PLAT], e->platform[0] ? e->platform : "?");
    lv_label_set_text(slots[SLOT_TYPE], e->type[0] ? e->type : "");

    // Extract the train number from `vehicle` (format "IC 2204"). The number
    // is whatever follows the first space; if there is no space, leave it
    // blank rather than echoing the type.
    const char *space = strchr(e->vehicle, ' ');
    lv_label_set_text(slots[SLOT_NUMBER], (space && space[1]) ? space + 1 : "");
}

static void clear_row(int row_idx, lv_obj_t *row)
{
    lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(row);
    if (!slots) return;
    s_via_state[row_idx].full_text[0] = '\0';
    s_via_state[row_idx].num_stops    = 0;
    s_via_state[row_idx].num_pages    = 0;
    lv_label_set_text(slots[SLOT_TIME], "");
    lv_obj_add_flag(slots[SLOT_DELAY_BOX], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(slots[SLOT_NEW_TIME], "");
    lv_label_set_text(slots[SLOT_DEST], "");
    lv_label_set_text(slots[SLOT_VEHICLE], "");
    lv_label_set_text(slots[SLOT_PLAT], "");
    lv_label_set_text(slots[SLOT_TYPE], "");
    lv_label_set_text(slots[SLOT_NUMBER], "");
    lv_obj_remove_style(slots[SLOT_DEST], &st_text_dest_cancel, 0);
}

// -------- Public API --------

esp_err_t ui_build(void)
{
    styles_init();

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_add_style(s_screen, &st_screen_bg, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    // Note: making s_screen clickable doesn't reliably catch taps in LVGL 9
    // when opaque children cover it. We attach the toggle handler to the
    // hdr and body widgets explicitly instead (see below).

    // ---- HEADER ----
    lv_obj_t *hdr = plain_obj(s_screen, &st_header, 0, 0, LCD_H_RES, HEADER_H);
    // Tap anywhere in the header (outside the logo, which has its own
    // handler) toggles Vertrek <-> Aankomst.
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, on_logo_tap, LV_EVENT_CLICKED, NULL);

    // Clock: HH / colon / MM as three labels. The colon's color toggles
    // every second between white (visible) and the header bg (invisible)
    // for a ticking-clock feel.
    s_header_hh  = label(hdr, &st_text_clock, "--", 8,  8);
    s_header_sep = label(hdr, &st_text_clock, ":",  40, 8);
    s_header_mm  = label(hdr, &st_text_clock, "--", 52, 8);

    // Single header title — shows "Vertrek" OR "Aankomst" depending on the
    // active mode. Toggled by tapping the NMBS logo (below), not by tapping
    // the title itself.
    s_header_mode = label(hdr, &st_text_mode, "Vertrek", 110, 8);

    // Wifi indicator (small, top-right inside header, sits left of the logo).
    s_header_wifi = label(hdr, &st_wifi_indicator, "", LCD_H_RES - 96, 12);

    // Freshness traffic-light dot. 6 px round, centered vertically in the
    // 40 px header, sits 8 px left of the wifi indicator. Starts RED until
    // main.c reports a successful fetch via ui_tick_freshness().
    s_freshness_dot = lv_obj_create(hdr);
    lv_obj_remove_style_all(s_freshness_dot);
    lv_obj_set_size(s_freshness_dot, 6, 6);
    lv_obj_set_pos(s_freshness_dot, LCD_H_RES - 108, 17);
    lv_obj_set_style_bg_color(s_freshness_dot, NMBS_RED, 0);
    lv_obj_set_style_bg_opa(s_freshness_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_freshness_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_freshness_dot, 0, 0);
    lv_obj_clear_flag(s_freshness_dot, LV_OBJ_FLAG_SCROLLABLE);

    // Weather chip — sits in the gap between the mode label and the
    // freshness dot. Hidden until ui_set_weather() is called with a valid
    // code. Plain off-white text in the same small font as the wifi
    // indicator so it doesn't fight the clock for attention.
    s_weather_label = lv_label_create(hdr);
    lv_obj_remove_style_all(s_weather_label);
    lv_obj_set_style_text_color(s_weather_label, NMBS_OFFWHITE, 0);
    lv_obj_set_style_text_font(s_weather_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_weather_label, LCD_H_RES - 180, 14);
    lv_label_set_text(s_weather_label, "");
    lv_obj_add_flag(s_weather_label, LV_OBJ_FLAG_HIDDEN);

    // Alert bar — full-width red strip under the header. The first line of
    // the active iRail service alert scrolls inside it. Hidden by default.
    s_alert_bar = plain_obj(s_screen, &st_delay_box,
                            0, HEADER_H, LCD_H_RES, 18);
    lv_obj_set_style_radius(s_alert_bar, 0, 0);
    s_alert_text = label(s_alert_bar, &st_text_delay, "", 8, 2);
    lv_obj_set_width(s_alert_text, LCD_H_RES - 16);
    lv_label_set_long_mode(s_alert_text, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_flag(s_alert_bar, LV_OBJ_FLAG_HIDDEN);

    // Actual NMBS/SNCB van de Velde B-in-oval, rasterised from the official
    // SVG at compile time (firmware/main/assets/nmbs_logo.c). 56x36 ARGB8888.
    // Tap-to-toggle: the logo doubles as a Vertrek <-> Aankomst switch, with
    // a generous extended click area so it's easy to hit.
    s_logo = lv_image_create(hdr);
    lv_image_set_src(s_logo, &nmbs_logo);
    lv_obj_set_pos(s_logo, LCD_H_RES - 60, 2);
    lv_obj_add_flag(s_logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_logo, 16);
    lv_obj_add_event_cb(s_logo, on_logo_tap, LV_EVENT_CLICKED, NULL);

    // ---- BODY (5 stacked rows) ----
    lv_obj_t *body = plain_obj(s_screen, &st_screen_bg,
                               0, BODY_TOP, LCD_H_RES, BODY_H);
    s_body = body;
    // Start the body HIDDEN. The overlay is up at boot (covers the screen)
    // and the body is only revealed by ui_hide_overlay once main.c has
    // verified Wi-Fi + a successful iRail fetch. This guarantees the user
    // never sees an empty or stale timetable, even for a single frame.
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    // Body is a big tap target, and we also flag each row clickable since
    // LVGL 9's click bubbling through opaque non-clickable children is
    // unreliable. Both paths invoke the same toggle handler.
    // Long-press on body OR header wipes Wi-Fi creds and reboots into the
    // AP provisioning flow (3 s hold; configured in bsp.c).
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, on_logo_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(body, on_long_press_reset, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(hdr,  on_long_press_reset, LV_EVENT_LONG_PRESSED, NULL);
    for (int i = 0; i < N_ROWS; i++) {
        s_rows[i] = build_row(body, i);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_rows[i], on_logo_tap, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(s_rows[i], on_long_press_reset, LV_EVENT_LONG_PRESSED, NULL);
    }

    apply_header_title_locked();

    // ---- WI-FI STATUS OVERLAY (full-screen, hidden by default) ----
    // Sits on top of the body so it can cover both the header and the
    // rows. Pure black bg so there's never any ambiguity between this and
    // the navy timetable underneath.
    s_overlay = plain_obj(s_screen, &st_overlay_bg, 0, 0, LCD_H_RES, LCD_V_RES);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Make the overlay itself clickable so the 3-second long-press wipes
    // NVS even when the splash / "Verbinding maken..." screen is up —
    // otherwise the user can't recover from a wrong-password boot loop
    // because the body underneath is occluded.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, on_long_press_reset, LV_EVENT_LONG_PRESSED, NULL);
    // Big NMBS logo, centred a bit above the middle.
    lv_obj_t *ovl_logo = lv_image_create(s_overlay);
    lv_image_set_src(ovl_logo, &nmbs_logo);
    lv_obj_align(ovl_logo, LV_ALIGN_CENTER, 0, -70);
    // Headline message below the logo, centred.
    s_overlay_msg = lv_label_create(s_overlay);
    lv_obj_remove_style_all(s_overlay_msg);
    lv_obj_add_style(s_overlay_msg, &st_text_mode, 0);
    lv_label_set_text(s_overlay_msg, "");
    lv_obj_align(s_overlay_msg, LV_ALIGN_CENTER, 0, -10);
    // Two-line secondary panel used by the AP-mode provisioning screen.
    s_overlay_sub1 = lv_label_create(s_overlay);
    lv_obj_remove_style_all(s_overlay_sub1);
    lv_obj_set_style_text_color(s_overlay_sub1, NMBS_OFFWHITE, 0);
    lv_obj_set_style_text_font(s_overlay_sub1, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_overlay_sub1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_overlay_sub1, "");
    lv_obj_align(s_overlay_sub1, LV_ALIGN_CENTER, 0, 26);
    s_overlay_sub2 = lv_label_create(s_overlay);
    lv_obj_remove_style_all(s_overlay_sub2);
    lv_obj_set_style_text_color(s_overlay_sub2, NMBS_YELLOW, 0);
    lv_obj_set_style_text_font(s_overlay_sub2, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_overlay_sub2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_overlay_sub2, "");
    lv_obj_align(s_overlay_sub2, LV_ALIGN_CENTER, 0, 56);

    // Small build-info panel at the bottom: app + version, build date,
    // ESP-IDF version, developer, source URL. Anchored to the bottom so
    // it stays out of the way of the main message even if message wraps.
    s_overlay_info = lv_label_create(s_overlay);
    lv_obj_remove_style_all(s_overlay_info);
    lv_obj_set_style_text_color(s_overlay_info, NMBS_GREY, 0);
    lv_obj_set_style_text_font(s_overlay_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_overlay_info, LV_TEXT_ALIGN_LEFT, 0);
    // Width leaves a 90 px gap on the right for the QR code below. Without
    // this, the centered text drifts under the QR and overlaps it.
    lv_obj_set_width(s_overlay_info, LCD_H_RES - 110);
    {
        char info[256];
        // ASCII-only — Montserrat 14 doesn't have U+00B7 middle dot or
        // U+2022 bullet glyphs in its default range, which renders as the
        // dreaded white-outlined square.
        snprintf(info, sizeof(info),
                 "%s v%s\n"
                 "build %s %s  |  ESP-IDF " IDF_VER "\n"
                 "by %s\n"
                 "%s",
                 APP_NAME, APP_VERSION,
                 __DATE__, __TIME__,
                 APP_DEVELOPER,
                 APP_HOMEPAGE_SHORT);
        lv_label_set_text(s_overlay_info, info);
    }
    lv_obj_align(s_overlay_info, LV_ALIGN_BOTTOM_LEFT, 10, -8);

    // Scannable QR code linking back to the project's GitHub page.
    // Anchored to the bottom-right of the splash, sized 70x70 so it
    // pulls focus without crowding the build-info block. White-on-dark
    // for max scan contrast (LVGL's qrcode widget takes BG and FG
    // colours explicitly).
    lv_obj_t *qr = lv_qrcode_create(s_overlay);
    lv_qrcode_set_size(qr, 70);
    lv_qrcode_set_dark_color(qr, lv_color_white());
    lv_qrcode_set_light_color(qr, lv_color_black());
    lv_qrcode_update(qr, APP_HOMEPAGE, strlen(APP_HOMEPAGE));
    lv_obj_align(qr, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    // Overlay starts VISIBLE — the splash is up from the very first paint,
    // before app_main has even configured a message. This pairs with
    // s_body starting HIDDEN to guarantee no timetable flash at boot.
    lv_obj_move_foreground(s_overlay);

    // Page-flip timer for the via stops. Runs inside the LVGL task so it
    // already holds the lock — but we re-acquire defensively in the cb.
    lv_timer_create(marquee_tick, MARQUEE_INTERVAL_MS, NULL);
    // 500 ms = colon flips every half-second so a full HH:MM:on -> off
    // cycle is one second per the user's preference.
    lv_timer_create(clock_tick_cb, 500, NULL);

    bsp_lvgl_unlock();
    return ESP_OK;
}

void ui_show_overlay(const char *message)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_overlay) {
        if (message && s_overlay_msg) {
            lv_label_set_text(s_overlay_msg, message);
        }
        // Clear the two-line setup panel so the simple overlay variant
        // doesn't show leftover provisioning text.
        if (s_overlay_sub1) lv_label_set_text(s_overlay_sub1, "");
        if (s_overlay_sub2) lv_label_set_text(s_overlay_sub2, "");
        // Hide the timetable body FIRST and in the same lock as showing
        // the overlay, so the user can never see a stale row underneath a
        // partly-painted overlay during a state transition.
        if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_overlay_visible = true;
        ui_full_redraw_locked();
    }
    bsp_lvgl_unlock();
}

void ui_show_provisioning(const char *ap_ssid, const char *url)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_overlay && s_overlay_msg) {
        lv_label_set_text(s_overlay_msg, "Setup mode");
        if (s_overlay_sub1) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Connect to Wi-Fi: %s",
                     ap_ssid ? ap_ssid : "?");
            lv_label_set_text(s_overlay_sub1, buf);
        }
        if (s_overlay_sub2) {
            lv_label_set_text(s_overlay_sub2, url ? url : "");
        }
        if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_overlay_visible = true;
        ui_full_redraw_locked();
    }
    bsp_lvgl_unlock();
}

void ui_hide_overlay(void)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        // Reveal the body in the same lock so the populated timetable
        // appears as a single atomic transition — no gap of empty screen
        // or stale rows between overlay-hide and body-show.
        if (s_body) lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        s_overlay_visible = false;
        ui_full_redraw_locked();
    }
    bsp_lvgl_unlock();
}

void ui_set_mode(ui_mode_t mode)
{
    if (!bsp_lvgl_lock(500)) return;
    s_mode = mode;
    apply_header_title_locked();
    apply_render_locked();
    ui_full_redraw_locked();   // clean transition between Vertrek and Aankomst
    bsp_lvgl_unlock();
}

void ui_set_boards(const irail_board_t *deps, const irail_board_t *arrs)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG_UI, "ui_set_boards: lock timeout");
        return;
    }
    s_dep_board = deps;
    s_arr_board = arrs;
    apply_render_locked();
    bsp_lvgl_unlock();
}

void ui_set_alert(const char *headline)
{
    if (!bsp_lvgl_lock(50)) return;
    if (s_alert_bar && s_alert_text) {
        if (headline && headline[0]) {
            lv_label_set_text(s_alert_text, headline);
            lv_obj_clear_flag(s_alert_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_alert_bar);
        } else {
            lv_obj_add_flag(s_alert_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    bsp_lvgl_unlock();
}

// WMO weather codes: 0 clear, 1-3 partly cloudy, 45/48 fog, 51..67 drizzle/rain,
// 71..77 snow, 80..82 showers, 85/86 snow showers, 95..99 thunder.
// We only need a single-character glyph from the basic ASCII range that
// Montserrat ships with — emoji isn't an option here.
static char weather_glyph(int code)
{
    if (code <  0)                          return '?';
    if (code == 0)                          return '*';   // sun
    if (code >= 1  && code <= 3)            return '~';   // partly cloudy
    if (code == 45 || code == 48)           return '=';   // fog
    if (code >= 51 && code <= 67)           return '.';   // rain
    if (code >= 71 && code <= 77)           return '#';   // snow
    if (code >= 80 && code <= 82)           return '.';   // showers
    if (code >= 85 && code <= 86)           return '#';   // snow showers
    if (code >= 95)                         return '!';   // thunder
    return '?';
}

void ui_set_weather(float temp_c, int weather_code)
{
    if (!bsp_lvgl_lock(50)) return;
    if (s_weather_label) {
        if (weather_code < 0) {
            lv_obj_add_flag(s_weather_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%c %d", weather_glyph(weather_code),
                     (int)(temp_c + (temp_c >= 0 ? 0.5f : -0.5f)));
            lv_label_set_text(s_weather_label, buf);
            lv_obj_clear_flag(s_weather_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    bsp_lvgl_unlock();
}

void ui_tick_freshness(time_t last_success_unix)
{
    // Only repaint the dot when its colour actually changes. Calling
    // lv_obj_set_style_bg_color every second — even with the same value —
    // marks the object dirty and forces a partial repaint, which on this
    // i80-DMA pipeline very occasionally bleeds stale pixels (the dreaded
    // red/green flashes in the via and clock areas).
    static lv_color_t s_last_color = {0};
    static bool       s_last_color_valid = false;

    lv_color_t color = NMBS_RED;
    if (last_success_unix != 0) {
        time_t age = time(NULL) - last_success_unix;
        if (age < 0) age = 0;   // clock skew defence
        // Aggressive thresholds (per user choice):
        //   green  <= 60 s
        //   yellow 60 s < age <= 120 s
        //   red    > 120 s
        if (age <= 60)        color = lv_color_hex(0x33CC33);  // green
        else if (age <= 120)  color = NMBS_YELLOW;
        else                  color = NMBS_RED;
    }

    if (s_last_color_valid && lv_color_eq(color, s_last_color)) {
        return;     // no visible change — don't dirty the dot
    }

    if (!bsp_lvgl_lock(50)) return;
    if (s_freshness_dot) {
        lv_obj_set_style_bg_color(s_freshness_dot, color, 0);
        s_last_color = color;
        s_last_color_valid = true;
    }
    bsp_lvgl_unlock();
}

void ui_tick_status(bool wifi_ok)
{
    // Clock + colon are driven by an LVGL timer (see clock_tick_cb) so
    // they keep ticking even while main_loop_task is blocked on a long
    // iRail TLS handshake. This call now only refreshes the wifi
    // indicator, which is cheap and edge-driven from main_loop_task.
    if (!bsp_lvgl_lock(50)) return;
    if (s_header_wifi) {
        lv_label_set_text(s_header_wifi,
            wifi_ok ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
    }
    bsp_lvgl_unlock();
}
