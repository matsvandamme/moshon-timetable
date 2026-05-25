// moshon-timetable — NMBS train timetable for Aalter station on WT32-SC01 Plus.
//
// Boot flow:
//   1. NVS init
//   2. Display + LVGL bring-up (bsp_init)
//   3. UI skeleton (ui_build) so the user immediately sees branding
//   4. Wi-Fi STA connect + SNTP
//   5. Periodic worker: every REFRESH_PERIOD_MS, fetch departures + arrivals
//      from iRail and update the LVGL boards.

#include "app_config.h"
#include "bsp.h"
#include "ui.h"
#include "wifi.h"
#include "irail.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>

static void refresh_task(void *arg)
{
    static irail_board_t deps;
    static irail_board_t arrs;

    while (1) {
        if (wifi_is_connected()) {
            time_t now = 0;

            esp_err_t r1 = irail_fetch_departures(&deps);
            esp_err_t r2 = irail_fetch_arrivals(&arrs);

            if (r1 == ESP_OK || r2 == ESP_OK) {
                now = time(NULL);
                ui_update_boards(r1 == ESP_OK ? &deps : NULL,
                                 r2 == ESP_OK ? &arrs : NULL);
                ui_update_status(true, now);
            } else {
                ESP_LOGW(TAG_APP, "Both fetches failed");
                ui_update_status(true, now);
            }
        } else {
            ui_update_status(false, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_PERIOD_MS));
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
    ESP_ERROR_CHECK(ui_build());
    ui_update_status(false, 0);

    if (wifi_start_and_wait(20000) == ESP_OK) {
        time_sync_start();
        // Give SNTP a moment to land a real time before the first fetch tries
        // to label the refresh footer.
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGE(TAG_APP, "Wi-Fi did not come up in time — will keep retrying");
    }

    xTaskCreate(refresh_task, "refresh", 8 * 1024, NULL, 5, NULL);

    ESP_LOGI(TAG_APP, "app_main done — handing off to tasks");
}
