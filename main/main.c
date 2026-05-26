// moshon-timetable — NMBS train timetable (Vertrek + Aankomst) for the
// active station, alternating like the real in-station NMBS boards.

#include "app_config.h"
#include "bsp.h"
#include "ui.h"
#include "wifi.h"
#include "irail.h"
#include "irail_vehicle.h"
#include "stations.h"
#include "demo.h"
#include "cfg.h"
#include "i18n.h"
#include "provision_ap.h"
#include "settings_web.h"
#include "weather.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <string.h>
#include "esp_timer.h"

#define SPLASH_MIN_VISIBLE_MS  4000   // keep dev info readable even on fast Wi-Fi

#ifndef CONFIG_BOARD_TOGGLE_SECONDS
#define CONFIG_BOARD_TOGGLE_SECONDS 12
#endif

static irail_board_t s_deps;
static irail_board_t s_arrs;
static bool          s_deps_ok = false;
static bool          s_arrs_ok = false;

#define ROWS_TARGET     5         // matches N_ROWS in ui.c
#define PAGE_GAP_SEC    60        // ask for "last_entry + 1 minute" next page
#define PAGE_MAX_TRIES  4         // upper bound on extra HTTP calls per side

// Fetch `arrdep` and return the next ROWS_TARGET trains whose
// scheduled-time + delay is STILL IN THE FUTURE relative to wall-clock now.
//
// Starts from `from_time` (0 = now), pages forward, jumps the anchor an hour
// at a time if iRail returns nothing in a window (overnight lull). Caps at
// PAGE_MAX_TRIES iterations so a permanently-empty station never spams iRail.
//
// Crucially: any iRail-returned entry with actual_time < now is filtered out
// before we ever pass it to the UI, so the board can never show a past time.
static esp_err_t fill_board(const char *arrdep, time_t from_time, irail_board_t *out)
{
    time_t now = time(NULL);
    if (from_time == 0) from_time = now;

    out->count = 0;
    out->fetched_at = now;
    out->for_time = from_time;

    int tries = 0;
    time_t anchor = from_time;
    // Track whether AT LEAST ONE underlying HTTP request actually succeeded
    // (regardless of how many entries it yielded). Used to differentiate
    // "iRail returned 0 trains right now" (valid, ESP_OK) from "couldn't
    // reach iRail at all" (ESP_FAIL). The UI layer only authorises the
    // timetable to appear when at least one fetch genuinely succeeded.
    bool any_http_success = false;

    while (out->count < ROWS_TARGET && tries < PAGE_MAX_TRIES) {
        irail_board_t batch = {0};
        // First call: pass for_time=0 so iRail's "now" semantics kick in
        // (it tends to return more results than an explicit future anchor).
        esp_err_t r = irail_fetch(arrdep, (tries == 0 ? 0 : anchor), &batch);
        if (r != ESP_OK) break;
        any_http_success = true;

        size_t accepted_this_round = 0;
        for (size_t i = 0; i < batch.count && out->count < IRAIL_MAX_ENTRIES; i++) {
            const irail_entry_t *e = &batch.entries[i];
            time_t actual = e->scheduled + (time_t)e->delay_seconds;
            if (actual < now) continue;  // already left / arrived — never show

            bool dup = false;
            for (size_t j = 0; j < out->count; j++) {
                if (out->entries[j].scheduled == e->scheduled &&
                    strcmp(out->entries[j].vehicle, e->vehicle) == 0) {
                    dup = true; break;
                }
            }
            if (!dup) {
                out->entries[out->count++] = *e;
                accepted_this_round++;
            }
        }

        if (out->count >= ROWS_TARGET) break;

        // Pick the next anchor: continue past the last entry we got, or — if
        // the window was empty / fully in-the-past — leap forward an hour to
        // skip dead time (e.g. 02:30 -> 03:30 -> 04:30 until trains appear).
        if (batch.count > 0) {
            time_t last = batch.entries[batch.count - 1].scheduled;
            anchor = (last + PAGE_GAP_SEC > anchor) ? last + PAGE_GAP_SEC
                                                    : anchor + 60 * 60;
        } else {
            anchor += 60 * 60;
        }
        tries++;
    }

    if (tries > 0) {
        ESP_LOGI(TAG_APP, "paginated %s: %u future entries in %d fetches",
                 arrdep, (unsigned)out->count, tries);
    }
    return any_http_success ? ESP_OK : ESP_FAIL;
}

static void do_fetch(void)
{
#ifdef CONFIG_DEMO_MODE
    // Demo mode: skip iRail entirely, generate synthetic boards. Times are
    // anchored at the current clock so things always look "now-ish".
    time_t now = time(NULL);
    demo_fill_departures(&s_deps, now);
    demo_fill_arrivals  (&s_arrs, now);
    s_deps_ok = true;
    s_arrs_ok = true;
    return;
#endif

    if (!wifi_is_connected()) return;

    // fill_board paginates forward and filters past entries internally, so
    // we no longer need a separate "tomorrow midnight" branch. Whether it's
    // 14:00 or 02:28 AM, this returns the next ROWS_TARGET trains whose
    // scheduled-or-delayed time is still in the future.
    s_deps_ok = (fill_board("departure", 0, &s_deps) == ESP_OK);
    s_arrs_ok = (fill_board("arrival",   0, &s_arrs) == ESP_OK);

    if (!s_deps_ok && !s_arrs_ok) {
        ESP_LOGW(TAG_APP, "Both fetches failed");
    }

    // Enrich each entry with its "via X, Y, Z" subtitle. The first refresh
    // hits iRail per train; subsequent refreshes are mostly cache hits.
    // Errors here are non-fatal — the row falls back to showing the vehicle
    // code in the subtitle slot.
    const char *origin = station_get_active()->display_name;
    if (s_deps_ok) {
        for (size_t i = 0; i < s_deps.count; i++) {
            // Departures: yellow label already shows the terminus, so omit
            // it from the via list to avoid duplication.
            irail_vehicle_get_via(s_deps.entries[i].vehicle_id,
                                  s_deps.entries[i].scheduled,
                                  origin,
                                  /* include_terminus = */ false,
                                  s_deps.entries[i].via,
                                  sizeof(s_deps.entries[i].via));
        }
    }
    if (s_arrs_ok) {
        for (size_t i = 0; i < s_arrs.count; i++) {
            // Arrivals: yellow label shows the train's ORIGIN, so the via
            // list runs from "stop after us" all the way to (and including)
            // the final destination — useful for onward-journey planning.
            irail_vehicle_get_via(s_arrs.entries[i].vehicle_id,
                                  s_arrs.entries[i].scheduled,
                                  origin,
                                  /* include_terminus = */ true,
                                  s_arrs.entries[i].via,
                                  sizeof(s_arrs.entries[i].via));
        }
    }
}

// True iff iRail returned at least one entry on the most recent fetch.
// This is the gate for revealing the timetable — Wi-Fi alone isn't enough,
// the timetable only appears once a successful fetch has produced data.
static bool boards_have_data(void)
{
    return (s_deps_ok && s_deps.count > 0) ||
           (s_arrs_ok && s_arrs.count > 0);
}

// Returns true if the current local hour falls inside the user's
// configured night-dim window. Window can wrap midnight: 22..7 means
// "dim from 10 PM through 7 AM".
static bool in_dim_window(uint8_t start, uint8_t end)
{
    if (start == end) return false;       // disabled
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int h = tm.tm_hour;
    if (start < end) return h >= start && h < end;
    return h >= start || h < end;        // wraps midnight
}

// One-second state machine. Computes the desired overlay state from
// (wifi_ok, last_fetch_succeeded, boards_have_data) and only transitions
// the UI when the desired state differs from the current state — so the
// overlay never re-paints redundantly each tick. The timetable body is
// hidden any time the overlay is up (handled atomically inside
// ui_show_overlay / ui_hide_overlay), so the user can never see an empty
// or stale board behind a not-yet-fully-painted overlay.
static void main_loop_task(void *arg)
{
    time_t last_fetch = 0;
    // Tracks the unix time of the most recent fetch that actually returned
    // data. Drives the header traffic-light dot. Stays 0 until iRail
    // delivers at least one entry, which paints the dot red on screen.
    time_t last_success = 0;
    bool   was_wifi   = wifi_is_connected();
    bool   overlay_up = true;       // ui_build leaves the boot splash visible
    const char *overlay_msg = i18n_text(TR_CONNECTING);
    bool   was_dimmed = false;      // tracks current backlight on/off state
    time_t last_weather_fetch = 0;

    while (1) {
        time_t now = time(NULL);
        bool wifi_ok = wifi_is_connected();
        bool just_reconnected = (wifi_ok && !was_wifi);
        was_wifi = wifi_ok;

        // Fetch when: (a) we've never fetched, (b) wifi just came back, or
        // (c) refresh period elapsed. All gated on wifi being up.
        bool should_fetch = wifi_ok &&
                            (last_fetch == 0 ||
                             just_reconnected ||
                             (now - last_fetch >= CONFIG_REFRESH_PERIOD_SECONDS));
        if (should_fetch) {
            do_fetch();
            last_fetch = now;
            // Push to the UI regardless — render_entry handles empty arrays
            // gracefully and the body is hidden behind the overlay anyway
            // until the data_ok gate flips below.
            ui_set_boards(s_deps_ok ? &s_deps : NULL,
                          s_arrs_ok ? &s_arrs : NULL);
            // Only count it as a "successful" fetch (for the freshness dot)
            // when iRail actually returned data — an HTTP-OK-but-empty
            // response shouldn't reset the staleness clock.
            if ((s_deps_ok && s_deps.count > 0) ||
                (s_arrs_ok && s_arrs.count > 0)) {
                last_success = now;
            }
        }

        // Desired state: timetable only visible when wifi is associated AND
        // the most recent fetch actually returned at least one entry.
        bool show_timetable = wifi_ok && boards_have_data();

        if (show_timetable) {
            if (overlay_up) {
                ui_hide_overlay();
                overlay_up = false;
                overlay_msg = NULL;
            }
        } else {
            // Pick the right message for *why* we're hiding the timetable.
            const char *new_msg;
            if (!wifi_ok) {
                new_msg = i18n_text(TR_RECONNECTING);
            } else if (last_fetch == 0) {
                new_msg = i18n_text(TR_CONNECTING);
            } else {
                new_msg = i18n_text(TR_FETCHING_DATA);
            }
            if (!overlay_up || overlay_msg != new_msg) {
                ui_show_overlay(new_msg);
                overlay_up = true;
                overlay_msg = new_msg;
            }
        }

        ui_tick_status(wifi_ok);
        ui_tick_freshness(last_success);

        // Alert banner: surface the freshest non-empty alert from either
        // board, clear it when neither has one.
        const char *alert = "";
        if (s_deps_ok && s_deps.alert_headline[0]) alert = s_deps.alert_headline;
        else if (s_arrs_ok && s_arrs.alert_headline[0]) alert = s_arrs.alert_headline;
        ui_set_alert(alert);

        // Auto-dim: read the user's NVS-saved window each tick (cheap NVS
        // read; user changes take effect within a second). When inside the
        // window, force backlight to 0; otherwise restore daytime brightness.
        uint8_t dim_s = 0, dim_e = 0;
        cfg_load_dim_hours(&dim_s, &dim_e);
        bool should_dim = in_dim_window(dim_s, dim_e);
        if (should_dim != was_dimmed) {
            bsp_set_backlight(should_dim ? 0 : 80);
            was_dimmed = should_dim;
        }

        // Weather: refresh once every 10 min via Open-Meteo. Cheap enough
        // not to throttle iRail. Skipped entirely if no coords configured.
        if (wifi_ok && weather_enabled() &&
            (last_weather_fetch == 0 || (now - last_weather_fetch) >= 600)) {
            float t; int code;
            if (weather_fetch(&t, &code) == ESP_OK) {
                ui_set_weather(t, code);
            }
            last_weather_fetch = now;
        } else if (!weather_enabled()) {
            ui_set_weather(0, -1);   // hides the chip if user unsets coords
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG_APP, "%s %s starting", APP_NAME, APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(bsp_init());
    cfg_init();
    i18n_init();                       // load saved language (default NL)
    ESP_ERROR_CHECK(ui_build());
    ui_set_mode(UI_MODE_DEPARTURES);   // initial tab
    ui_tick_status(false);

#ifndef CONFIG_DEMO_MODE
    // No Wi-Fi creds in NVS? Bring up the SoftAP captive portal and stay
    // parked here forever — the /save handler reboots the chip once the
    // user submits the form. Critically: do NOT fall through to STA mode
    // on AP-start failure; otherwise a brief AP glitch would cause us to
    // silently re-join whatever the previous network was (silently
    // undoing the user's long-press wipe).
    if (!cfg_has_wifi()) {
        ui_show_provisioning(PROVISION_AP_SSID, PROVISION_AP_URL);
        esp_err_t apr = provision_ap_start();
        if (apr != ESP_OK) {
            ESP_LOGE(TAG_APP, "AP provisioning failed: %s",
                     esp_err_to_name(apr));
            ui_show_overlay("Setup mode error");
        }
        while (1) vTaskDelay(pdMS_TO_TICKS(60000));  // hold the splash, no fall-through
    }
#endif

    // Show the connect-to-Wi-Fi splash while we attempt association. The
    // overlay stays up until association actually succeeds — whether that
    // happens in this 20 s window or several minutes later via the
    // background reconnect logic in wifi.c. main_loop_task hides the
    // overlay on the disconnected->connected edge.
    int64_t splash_started_us = esp_timer_get_time();
    ui_show_overlay(i18n_text(TR_CONNECTING));

    esp_err_t wres = wifi_start_and_wait(20000);
    if (wres == ESP_OK) {
        // time_sync_start() now blocks up to 8 s for the first SNTP sync
        // before returning, so the on-screen clock + iRail "date" param
        // are correct from the first paint.
        time_sync_start();
        // Bring up the on-network settings page + OTA endpoint + mDNS so
        // the user can configure the device from a browser on their LAN
        // without ever needing the AP-mode portal again.
        settings_web_start();
        // Mark this OTA boot as valid so the bootloader doesn't roll back
        // to the previous slot on the next reset. No-op for the factory
        // image; only meaningful after the user has done an OTA update.
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG_APP, "OTA: marking new image valid");
                esp_ota_mark_app_valid_cancel_rollback();
            }
        }
    } else {
        ESP_LOGW(TAG_APP, "Wi-Fi not up after 20 s — splash stays visible, retries continue");
    }

    // Always hold the splash for at least SPLASH_MIN_VISIBLE_MS so the build
    // info is readable even when Wi-Fi joins fast.
    int64_t elapsed_ms = (esp_timer_get_time() - splash_started_us) / 1000;
    if (elapsed_ms < SPLASH_MIN_VISIBLE_MS) {
        vTaskDelay(pdMS_TO_TICKS(SPLASH_MIN_VISIBLE_MS - (int)elapsed_ms));
    }

    // Leave the splash up here — main_loop_task drops it the moment iRail
    // returns at least one entry. Wi-Fi alone isn't enough: the user wants
    // to see the populated timetable before the overlay clears.
    (void)wres;

    // Pin the network/UI loop to core 1 alongside LVGL. CPU 0 stays free for
    // Wi-Fi, lwIP and IDLE0, so the task watchdog doesn't trip during the
    // multi-second mbedtls TLS handshakes against api.irail.be.
    //
    // Stack: 16 KB. The first liveboard fetch combines a fresh mbedtls TLS
    // handshake (heavy stack usage) with build_via_page / render_entry which
    // each push 2 * IRAIL_VIA_LEN bytes on the stack. With IRAIL_VIA_LEN at
    // 256 those buffers are 512 bytes each and 8 KB overflows during the
    // first fetch — confirmed via "***ERROR*** A stack overflow in task loop".
    xTaskCreatePinnedToCore(main_loop_task, "loop", 16 * 1024, NULL, 3, NULL, 1);

    ESP_LOGI(TAG_APP, "app_main done");
}
