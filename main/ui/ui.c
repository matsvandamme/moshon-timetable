#include "ui.h"
#include "nmbs_theme.h"
#include "bsp.h"
#include "app_config.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// ---------- Geometry (480 x 320 landscape) ----------
#define HEADER_H       44
#define FOOTER_H       24
#define BOARD_TOP      HEADER_H
#define BOARD_H        (LCD_V_RES - HEADER_H - FOOTER_H)
#define COL_W          (LCD_H_RES / 2)
#define ROW_H          26
#define ROWS_PER_COL   ((BOARD_H - 28) / ROW_H)   // minus column header

// ---------- Widget handles ----------
static lv_obj_t *s_screen        = NULL;
static lv_obj_t *s_header_time   = NULL;
static lv_obj_t *s_col_dep       = NULL;
static lv_obj_t *s_col_arr       = NULL;
static lv_obj_t *s_dep_rows[ROWS_PER_COL] = {0};
static lv_obj_t *s_arr_rows[ROWS_PER_COL] = {0};
static lv_obj_t *s_footer_wifi   = NULL;
static lv_obj_t *s_footer_refresh = NULL;

// ---------- Styles ----------
static lv_style_t st_screen_bg;
static lv_style_t st_header;
static lv_style_t st_header_title;
static lv_style_t st_logo_box;
static lv_style_t st_col;
static lv_style_t st_col_header;
static lv_style_t st_row;
static lv_style_t st_row_alt;
static lv_style_t st_row_cancelled;
static lv_style_t st_footer;
static lv_style_t st_text_white;
static lv_style_t st_text_white_bold;
static lv_style_t st_text_yellow;
static lv_style_t st_text_red;
static lv_style_t st_text_grey;

static void styles_init(void)
{
    lv_style_init(&st_screen_bg);
    lv_style_set_bg_color(&st_screen_bg, NMBS_BLUE_DARK);
    lv_style_set_bg_opa(&st_screen_bg, LV_OPA_COVER);
    lv_style_set_text_color(&st_screen_bg, NMBS_WHITE);
    lv_style_set_pad_all(&st_screen_bg, 0);
    lv_style_set_border_width(&st_screen_bg, 0);

    lv_style_init(&st_header);
    lv_style_set_bg_color(&st_header, NMBS_BLUE_DEEP);
    lv_style_set_bg_opa(&st_header, LV_OPA_COVER);
    lv_style_set_border_width(&st_header, 0);
    lv_style_set_pad_left(&st_header, 8);
    lv_style_set_pad_right(&st_header, 8);
    lv_style_set_pad_top(&st_header, 4);
    lv_style_set_pad_bottom(&st_header, 4);
    lv_style_set_radius(&st_header, 0);

    lv_style_init(&st_header_title);
    lv_style_set_text_font(&st_header_title, &lv_font_montserrat_24);
    lv_style_set_text_color(&st_header_title, NMBS_WHITE);

    lv_style_init(&st_logo_box);
    lv_style_set_bg_color(&st_logo_box, NMBS_YELLOW);
    lv_style_set_bg_opa(&st_logo_box, LV_OPA_COVER);
    lv_style_set_text_color(&st_logo_box, NMBS_BLUE_DEEP);
    lv_style_set_text_font(&st_logo_box, &lv_font_montserrat_28);
    lv_style_set_radius(&st_logo_box, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&st_logo_box, 0);
    lv_style_set_border_width(&st_logo_box, 0);

    lv_style_init(&st_col);
    lv_style_set_bg_color(&st_col, NMBS_BLUE_DARK);
    lv_style_set_bg_opa(&st_col, LV_OPA_COVER);
    lv_style_set_border_width(&st_col, 0);
    lv_style_set_pad_all(&st_col, 0);
    lv_style_set_radius(&st_col, 0);

    lv_style_init(&st_col_header);
    lv_style_set_bg_color(&st_col_header, NMBS_BLUE_MED);
    lv_style_set_bg_opa(&st_col_header, LV_OPA_COVER);
    lv_style_set_text_color(&st_col_header, NMBS_WHITE);
    lv_style_set_text_font(&st_col_header, &lv_font_montserrat_16);
    lv_style_set_pad_left(&st_col_header, 8);
    lv_style_set_pad_right(&st_col_header, 8);
    lv_style_set_pad_top(&st_col_header, 4);
    lv_style_set_pad_bottom(&st_col_header, 4);
    lv_style_set_border_width(&st_col_header, 0);
    lv_style_set_radius(&st_col_header, 0);

    lv_style_init(&st_row);
    lv_style_set_bg_opa(&st_row, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_pad_left(&st_row, 6);
    lv_style_set_pad_right(&st_row, 6);
    lv_style_set_pad_top(&st_row, 2);
    lv_style_set_pad_bottom(&st_row, 2);
    lv_style_set_radius(&st_row, 0);

    lv_style_init(&st_row_alt);
    lv_style_set_bg_color(&st_row_alt, lv_color_hex(0x122E5C));
    lv_style_set_bg_opa(&st_row_alt, LV_OPA_30);

    lv_style_init(&st_row_cancelled);
    lv_style_set_text_decor(&st_row_cancelled, LV_TEXT_DECOR_STRIKETHROUGH);
    lv_style_set_text_color(&st_row_cancelled, NMBS_GREY);

    lv_style_init(&st_footer);
    lv_style_set_bg_color(&st_footer, NMBS_BLACK);
    lv_style_set_bg_opa(&st_footer, LV_OPA_COVER);
    lv_style_set_text_color(&st_footer, NMBS_OFFWHITE);
    lv_style_set_text_font(&st_footer, &lv_font_montserrat_14);
    lv_style_set_pad_left(&st_footer, 6);
    lv_style_set_pad_right(&st_footer, 6);
    lv_style_set_pad_top(&st_footer, 2);
    lv_style_set_pad_bottom(&st_footer, 2);
    lv_style_set_border_width(&st_footer, 0);
    lv_style_set_radius(&st_footer, 0);

    lv_style_init(&st_text_white);
    lv_style_set_text_color(&st_text_white, NMBS_WHITE);
    lv_style_set_text_font(&st_text_white, &lv_font_montserrat_16);

    lv_style_init(&st_text_white_bold);
    lv_style_set_text_color(&st_text_white_bold, NMBS_WHITE);
    lv_style_set_text_font(&st_text_white_bold, &lv_font_montserrat_20);

    lv_style_init(&st_text_yellow);
    lv_style_set_text_color(&st_text_yellow, NMBS_YELLOW);
    lv_style_set_text_font(&st_text_yellow, &lv_font_montserrat_16);

    lv_style_init(&st_text_red);
    lv_style_set_text_color(&st_text_red, NMBS_RED);
    lv_style_set_text_font(&st_text_red, &lv_font_montserrat_16);

    lv_style_init(&st_text_grey);
    lv_style_set_text_color(&st_text_grey, NMBS_GREY);
    lv_style_set_text_font(&st_text_grey, &lv_font_montserrat_14);
}

// ---------- Helpers ----------

static lv_obj_t *plain_obj(lv_obj_t *parent, lv_style_t *style,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    if (style) lv_obj_add_style(o, style, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
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

// ---------- Build a single row (placeholder text, filled by update) ----------

static lv_obj_t *build_row(lv_obj_t *parent, int idx, bool alt)
{
    lv_obj_t *row = plain_obj(parent, &st_row, 0, idx * ROW_H, COL_W, ROW_H);
    if (alt) lv_obj_add_style(row, &st_row_alt, 0);

    // Layout columns inside row (positions in px from row's left):
    //   time:        0..52     (HH:MM)
    //   delay:      54..86     (+5')  optional, red
    //   platform:   88..112    (P:2A) yellow
    //   station:   116..(end)  white
    // (We attach by id via user_data so the updater can find children quickly.)
    label(row, &st_text_white_bold, "--:--", 0, 2);                 // time
    lv_obj_t *delay = label(row, &st_text_red,    "",      54, 4);   // delay (initially hidden)
    lv_obj_t *plat  = label(row, &st_text_yellow, "P:?",   88, 4);
    lv_obj_t *stn   = label(row, &st_text_white,  "...",  116, 4);
    lv_label_set_long_mode(stn, LV_LABEL_LONG_DOT);
    lv_obj_set_width(stn, COL_W - 116 - 6);

    // Stash children pointers in row user_data: array of [time, delay, plat, stn]
    static const int N = 4;
    lv_obj_t **slots = lv_malloc_zeroed(sizeof(lv_obj_t *) * N);
    slots[0] = lv_obj_get_child(row, 0);  // time
    slots[1] = delay;
    slots[2] = plat;
    slots[3] = stn;
    lv_obj_set_user_data(row, slots);
    return row;
}

static void format_time_local(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void render_entry(lv_obj_t *row, const irail_entry_t *e)
{
    if (!row) return;
    lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(row);
    if (!slots) return;

    char timebuf[8];
    format_time_local(e->scheduled, timebuf, sizeof(timebuf));
    lv_label_set_text(slots[0], timebuf);

    if (e->canceled) {
        lv_label_set_text(slots[1], "CANC");
    } else if (e->delay_seconds >= 60) {
        char db[12];
        snprintf(db, sizeof(db), "+%lu'", (unsigned long)(e->delay_seconds / 60));
        lv_label_set_text(slots[1], db);
    } else {
        lv_label_set_text(slots[1], "");
    }

    char pb[12];
    snprintf(pb, sizeof(pb), "P:%s", e->platform[0] ? e->platform : "?");
    lv_label_set_text(slots[2], pb);

    char sb[IRAIL_FIELD_LEN + 16];
    snprintf(sb, sizeof(sb), "%s  %s", e->other_station, e->vehicle);
    lv_label_set_text(slots[3], sb);
}

static void clear_row(lv_obj_t *row)
{
    if (!row) return;
    lv_obj_t **slots = (lv_obj_t **)lv_obj_get_user_data(row);
    if (!slots) return;
    lv_label_set_text(slots[0], "--:--");
    lv_label_set_text(slots[1], "");
    lv_label_set_text(slots[2], "");
    lv_label_set_text(slots[3], "");
}

// ---------- Public API ----------

esp_err_t ui_build(void)
{
    styles_init();

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_add_style(s_screen, &st_screen_bg, 0);

    // ---- HEADER ----
    lv_obj_t *hdr = plain_obj(s_screen, &st_header, 0, 0, LCD_H_RES, HEADER_H);

    // NMBS "B" logo mark (yellow circle + dark blue B) — abstract, not the
    // official logo (which we cannot redistribute).
    lv_obj_t *logo = plain_obj(hdr, &st_logo_box, 6, 4, 36, 36);
    lv_obj_t *blab = label(logo, NULL, "B", 0, 0);
    lv_obj_add_style(blab, &st_logo_box, 0);
    lv_obj_center(blab);

    // Station name (large, white)
    label(hdr, &st_header_title, "Aalter", 50, 8);

    // Live clock (right-aligned, big)
    s_header_time = label(hdr, &st_header_title, "--:--", LCD_H_RES - 90, 8);

    // ---- BOARDS ----
    s_col_dep = plain_obj(s_screen, &st_col, 0,    BOARD_TOP, COL_W, BOARD_H);
    s_col_arr = plain_obj(s_screen, &st_col, COL_W, BOARD_TOP, COL_W, BOARD_H);

    // Column headers
    lv_obj_t *dep_hdr = plain_obj(s_col_dep, &st_col_header, 0, 0, COL_W, 28);
    label(dep_hdr, NULL, LV_SYMBOL_RIGHT "  VERTREK", 8, 4);

    lv_obj_t *arr_hdr = plain_obj(s_col_arr, &st_col_header, 0, 0, COL_W, 28);
    label(arr_hdr, NULL, LV_SYMBOL_LEFT "  AANKOMST", 8, 4);

    // Row containers — anchor at y=28 (under the header)
    lv_obj_t *dep_rows_area = plain_obj(s_col_dep, &st_col, 0, 28, COL_W, BOARD_H - 28);
    lv_obj_t *arr_rows_area = plain_obj(s_col_arr, &st_col, 0, 28, COL_W, BOARD_H - 28);

    for (int i = 0; i < ROWS_PER_COL; i++) {
        s_dep_rows[i] = build_row(dep_rows_area, i, (i & 1) == 1);
        s_arr_rows[i] = build_row(arr_rows_area, i, (i & 1) == 1);
    }

    // ---- FOOTER ----
    lv_obj_t *foot = plain_obj(s_screen, &st_footer, 0, LCD_V_RES - FOOTER_H,
                               LCD_H_RES, FOOTER_H);
    s_footer_wifi    = label(foot, NULL, LV_SYMBOL_WIFI " connecting...", 0, 4);
    s_footer_refresh = label(foot, NULL, "last: --:--",                   LCD_H_RES - 110, 4);

    bsp_lvgl_unlock();
    return ESP_OK;
}

static void update_clock(void)
{
    if (!s_header_time) return;
    time_t now = time(NULL);
    char buf[8];
    format_time_local(now, buf, sizeof(buf));
    lv_label_set_text(s_header_time, buf);
}

void ui_update_boards(const irail_board_t *deps, const irail_board_t *arrs)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG_UI, "ui_update_boards: lock timeout");
        return;
    }
    if (deps) {
        for (int i = 0; i < ROWS_PER_COL; i++) {
            if (i < (int)deps->count) render_entry(s_dep_rows[i], &deps->entries[i]);
            else                       clear_row  (s_dep_rows[i]);
        }
    }
    if (arrs) {
        for (int i = 0; i < ROWS_PER_COL; i++) {
            if (i < (int)arrs->count) render_entry(s_arr_rows[i], &arrs->entries[i]);
            else                       clear_row  (s_arr_rows[i]);
        }
    }
    update_clock();
    bsp_lvgl_unlock();
}

void ui_update_status(bool wifi_ok, time_t last_refresh)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_footer_wifi) {
        lv_label_set_text(s_footer_wifi,
            wifi_ok ? LV_SYMBOL_WIFI " online" : LV_SYMBOL_WARNING " offline");
    }
    if (s_footer_refresh) {
        char buf[24];
        if (last_refresh > 0) {
            char tb[8];
            format_time_local(last_refresh, tb, sizeof(tb));
            snprintf(buf, sizeof(buf), "last: %s  iRail", tb);
        } else {
            snprintf(buf, sizeof(buf), "last: --:--");
        }
        lv_label_set_text(s_footer_refresh, buf);
    }
    update_clock();
    bsp_lvgl_unlock();
}
