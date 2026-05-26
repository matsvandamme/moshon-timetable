#include "wifi.h"
#include "cfg.h"
#include "app_config.h"
// wifi_secrets.h is still consulted as a one-time migration path on first
// boot after this refactor, so a user who already had creds compiled in
// doesn't have to re-enter them via the AP form.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <time.h>

#define WIFI_BIT_CONNECTED   BIT0
#define WIFI_BIT_FAIL        BIT1
#define WIFI_MAX_RETRIES     5

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
static bool s_connected = false;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG_WIFI, "STA start, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                if (s_retry_count < WIFI_MAX_RETRIES) {
                    s_retry_count++;
                    ESP_LOGW(TAG_WIFI, "Disconnected — retry %d/%d", s_retry_count, WIFI_MAX_RETRIES);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG_WIFI, "Giving up after %d retries", WIFI_MAX_RETRIES);
                    xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_FAIL);
                }
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG_WIFI, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_CONNECTED);
    }
}

esp_err_t wifi_start_and_wait(uint32_t timeout_ms)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // Set the DHCP hostname so the device is easy to spot in the router's
    // client list. Must happen AFTER esp_netif_create_default_wifi_sta()
    // (the netif must exist) and BEFORE the DHCP DISCOVER goes out (i.e.
    // before esp_wifi_connect()). Uses APP_NAME = "moshon-timetable".
    if (sta_netif) {
        esp_err_t hn = esp_netif_set_hostname(sta_netif, APP_NAME);
        if (hn != ESP_OK) {
            ESP_LOGW(TAG_WIFI, "set_hostname failed: %s", esp_err_to_name(hn));
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    // Resolve credentials: NVS first; if empty, migrate from any compile-time
    // WIFI_SSID/WIFI_PASSWORD that isn't the placeholder; else fail (caller
    // is expected to enter AP-mode provisioning).
    char ssid_buf[CFG_FIELD_MAX] = {0};
    char pass_buf[CFG_FIELD_MAX] = {0};
    esp_err_t cerr = cfg_load_wifi(ssid_buf, sizeof(ssid_buf),
                                   pass_buf, sizeof(pass_buf));
    if (cerr != ESP_OK || ssid_buf[0] == '\0') {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
        const char *cs = WIFI_SSID;
        const char *cp = WIFI_PASSWORD;
        if (cs[0] && strstr(cs, "REPLACE_WITH") == NULL) {
            ESP_LOGW(TAG_WIFI, "Migrating compile-time creds to NVS");
            strncpy(ssid_buf, cs, sizeof(ssid_buf) - 1);
            strncpy(pass_buf, cp, sizeof(pass_buf) - 1);
            cfg_save_wifi(ssid_buf, pass_buf);
        }
#endif
        if (ssid_buf[0] == '\0') {
            ESP_LOGE(TAG_WIFI, "No Wi-Fi credentials available");
            return ESP_ERR_NOT_FOUND;
        }
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid,     ssid_buf, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass_buf, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_BIT_CONNECTED | WIFI_BIT_FAIL,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_BIT_CONNECTED) return ESP_OK;
    if (bits & WIFI_BIT_FAIL)      return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t time_sync_start(void)
{
    ESP_LOGI(TAG_WIFI, "Starting SNTP");
    // NTP_SERVER_1 may come from wifi_secrets.h on legacy local builds; if
    // the file is gone fall back to a sensible default.
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "be.pool.ntp.org"
#endif
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.start = true;
    cfg.sync_cb = NULL;
    esp_netif_sntp_init(&cfg);
    esp_netif_sntp_start();

    // Europe/Brussels — CET/CEST.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    return ESP_OK;
}
