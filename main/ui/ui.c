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
#include "esp_system.h"

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
#define MARQUEE_INTERVAL_MS  4000
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
// Wi-Fi status overlay (full-screen). Hidden by default.
static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_overlay_msg   = NULL;
static lv_obj_t *s_overlay_sub1  = NULL;   // secondary line (e.g. AP SSID)
static lv_obj_t *s_overlay_sub2  = NULL;   // secondary line (e.g. URL)
static lv_obj_t *s_overlay_info  = NULL;   // bottom build-info panel
static bool      s_overlay_visible = false;
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
        if (sz.x > COL_DEST_W && !first_on_page) {
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
                      (s_mode == UI_MODE_DEPARTURES) ? "Vertrek" : "Aankomst");
}

static void apply_render_locked(void)
{
    const irail_board_t *b = (s_mode == UI_MODE_DEPARTURES) ? s_dep_board : s_arr_board;
    for (int i = 0; i < N_ROWS; i++) {
        if (b && i < (int)b->count) render_entry(i, s_rows[i], &b->entries[i]);
        else                          clear_row(i, s_rows[i]);
    }
}

// Periodic timer (~MARQUEE_INTERVAL_MS): advances ALL multi-page rows in
// lockstep to the next page of their via stops. The page index is global so
// every multi-page row always swaps text at the exact same instant.
// Rows with one (or zero) pages stay static — single-via lines just keep
// reading "via X" while the busier rows around them cycle.
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
    s_global_page_tick++;

    for (int i = 0; i < N_ROWS; i++) {
        via_state_t *s = &s_via_state[i];
        if (s->num_pages <= 1) continue;            // 0 or 1 page: stay put
        lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(s_rows[i]);
        if (!slots || !slots[SLOT_VEHICLE]) continue;

        int page_idx = s_global_page_tick % s->num_pages;
        int start = s->page_starts[page_idx];
        char display[IRAIL_VIA_LEN * 2];
        build_via_page(s->full_text, start, display, sizeof(display));
        lv_label_set_text(slots[SLOT_VEHICLE], display);
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

// Tap the NMBS B-logo to toggle Vertrek <-> Aankomst. Runs inside the LVGL
// task which already holds the lock.
static void on_logo_tap(lv_event_t *e)
{
    (void)e;
    s_mode = (s_mode == UI_MODE_DEPARTURES) ? UI_MODE_ARRIVALS : UI_MODE_DEPARTURES;
    apply_header_title_locked();
    apply_render_locked();
}

// 3-second hold anywhere on the screen wipes Wi-Fi credentials from NVS
// and reboots into AP-mode provisioning. The hold time is set in bsp.c
// via lv_indev_set_long_press_time.
static void on_long_press_reset(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG_UI, "Long-press detected: erasing Wi-Fi creds + restarting");
    cfg_erase_wifi();
    // Visual cue before the chip resets — the overlay paints almost
    // immediately even if esp_restart is queued right after.
    ui_show_overlay("Reset...");
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

    // Destination column
    lv_obj_t *t_dest    = label(row, &st_text_dest,    "...", COL_DEST_X,  6);
    lv_label_set_long_mode(t_dest,    LV_LABEL_LONG_DOT);
    lv_obj_set_width(t_dest,    COL_DEST_W);
    // Via subtitle: just a static label. The page-flip timer above swaps
    // its text every few seconds to step through stops. No LVGL marquee.
    lv_obj_t *t_vehicle = label(row, &st_text_vehicle, "",    COL_DEST_X, 32);
    lv_label_set_long_mode(t_vehicle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t_vehicle, COL_DEST_W);

    // Platform yellow badge
    lv_obj_t *t_plat_box = plain_obj(row, &st_plat_box,
                                     COL_PLAT_X, (ROW_H - 38) / 2, COL_PLAT_W, 38);
    lv_obj_t *t_plat = label(t_plat_box, &st_text_plat, "?", 0, 0);
    lv_obj_set_width(t_plat, COL_PLAT_W);
    lv_obj_set_style_text_align(t_plat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(t_plat);

    // Train type / class
    lv_obj_t *t_type = label(row, &st_text_type, "", COL_TYPE_X, (ROW_H - 22) / 2);
    lv_obj_set_width(t_type, COL_TYPE_W);
    lv_obj_set_style_text_align(t_type, LV_TEXT_ALIGN_CENTER, 0);

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
        lv_label_set_text(slots[SLOT_DELAY_LBL], "AFG");
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
            // Show the page corresponding to the *current* global tick so
            // a new entry slots into the existing rotation cleanly.
            int idx = s_global_page_tick % vs->num_pages;
            build_via_page(vs->full_text, vs->page_starts[idx],
                           page, sizeof(page));
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
    lv_obj_set_style_text_align(s_overlay_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_overlay_info, LCD_H_RES - 20);
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
    lv_obj_align(s_overlay_info, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    // Page-flip timer for the via stops. Runs inside the LVGL task so it
    // already holds the lock — but we re-acquire defensively in the cb.
    lv_timer_create(marquee_tick, MARQUEE_INTERVAL_MS, NULL);

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
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_overlay_visible = true;
        // Force a full-screen invalidate AND synchronously flush so the
        // overlay actually replaces every pixel before this call returns
        // — otherwise partial-render artefacts from prior frames can leak.
        if (s_screen) lv_obj_invalidate(s_screen);
        lv_refr_now(NULL);
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
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_overlay_visible = true;
        if (s_screen) lv_obj_invalidate(s_screen);
        lv_refr_now(NULL);
    }
    bsp_lvgl_unlock();
}

void ui_hide_overlay(void)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        s_overlay_visible = false;
        // Same trick on the way out — invalidate + sync flush so the
        // timetable underneath is fully redrawn before this returns.
        if (s_screen) lv_obj_invalidate(s_screen);
        lv_refr_now(NULL);
    }
    bsp_lvgl_unlock();
}

void ui_set_mode(ui_mode_t mode)
{
    if (!bsp_lvgl_lock(500)) return;
    s_mode = mode;
    apply_header_title_locked();
    apply_render_locked();
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

void ui_tick_status(bool wifi_ok)
{
    static bool blink_on = false;
    blink_on = !blink_on;

    if (!bsp_lvgl_lock(50)) return;

    if (s_header_hh && s_header_mm && s_header_sep) {
        struct tm tm;
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        char hh[4], mm[4];
        snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
        snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
        lv_label_set_text(s_header_hh, hh);
        lv_label_set_text(s_header_mm, mm);
        // Colon: white on alternating ticks, header-bg-coloured on the
        // others so it visually disappears without altering layout.
        lv_obj_set_style_text_color(s_header_sep,
            blink_on ? NMBS_WHITE : NMBS_BLUE_DEEP, 0);
    }
    if (s_header_wifi) {
        lv_label_set_text(s_header_wifi,
            wifi_ok ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
    }
    bsp_lvgl_unlock();
}
