#include "wifi.h"
#include "cfg.h"
#include "app_config.h"
// wifi_secrets.h is intentionally NOT included any more — credentials live
// in NVS exclusively, populated by the AP-mode setup form. The old
// compile-time migration silently rewrote NVS on the next STA boot, which
// caused a long-press wipe to be undone within seconds.

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
// No retry cap: the user wants the "Verbinding maken..." splash to stay up
// until Wi-Fi actually associates. Capping retries would cause the radio to
// give up after a handful of disconnects (e.g. AP briefly down) and the
// device would sit on the splash forever with no further connection attempts.
// Now we reconnect on every WIFI_EVENT_STA_DISCONNECTED, indefinitely.

static EventGroupHandle_t s_wifi_event_group = NULL;
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
                ESP_LOGW(TAG_WIFI, "Disconnected — reconnecting");
                esp_wifi_connect();
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG_WIFI, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
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

    // Resolve credentials: NVS only. main.c is responsible for routing the
    // boot into AP-mode provisioning when NVS is empty, so by the time this
    // function is reached we expect creds to be there.
    char ssid_buf[CFG_FIELD_MAX] = {0};
    char pass_buf[CFG_FIELD_MAX] = {0};
    esp_err_t cerr = cfg_load_wifi(ssid_buf, sizeof(ssid_buf),
                                   pass_buf, sizeof(pass_buf));
    if (cerr != ESP_OK || ssid_buf[0] == '\0') {
        ESP_LOGE(TAG_WIFI, "No Wi-Fi credentials in NVS");
        return ESP_ERR_NOT_FOUND;
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
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "be.pool.ntp.org"
#endif
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
    // server_from_dhcp requires CONFIG_LWIP_DHCP_GET_NTP_SRV — gate on
    // that so the init doesn't fail and leave SNTP completely off when
    // someone (or sdkconfig overrides) flip the Kconfig back off.
#ifdef CONFIG_LWIP_DHCP_GET_NTP_SRV
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
#endif
    cfg.start = true;
    cfg.sync_cb = NULL;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_sntp_start();

    // Europe/Brussels — CET/CEST.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    // Block briefly (up to 8 s) for the first SYNC so on-screen times
    // are correct from the very first paint, instead of showing 01:00 or
    // similar until the next iRail fetch happens to land after sync.
    int retry = 0;
    const int retry_max = 16;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500)) == ESP_ERR_TIMEOUT
           && ++retry < retry_max) {
        // keep waiting
    }
    if (retry >= retry_max) {
        ESP_LOGW(TAG_WIFI, "SNTP first sync did not land in 8 s; clock may lag");
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ESP_LOGI(TAG_WIFI, "SNTP synced, local time = %s", buf);
    }
    return ESP_OK;
}
