// NVS-backed config store. Single namespace "moshon", string-keyed.
//
// Wi-Fi credentials and station selection persist across reboots. The
// AP-mode provisioning flow writes here; the normal boot flow reads from
// here first and only falls back to compile-time defaults on cache miss.

#include "cfg.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define NS       "moshon"
#define K_SSID   "wifi_ssid"
#define K_PASS   "wifi_pass"
#define K_STAT   "station"
#define K_LANG   "language"

static const char *TAG = "cfg";

esp_err_t cfg_init(void)
{
    // nvs_flash_init() is already called from app_main; this is reserved
    // for future per-module initialisation.
    return ESP_OK;
}

esp_err_t cfg_load_wifi(char *ssid, size_t ssid_len,
                        char *pass, size_t pass_len)
{
    if (!ssid || ssid_len == 0 || !pass || pass_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t l = ssid_len;
    err = nvs_get_str(h, K_SSID, ssid, &l);
    if (err == ESP_OK) {
        l = pass_len;
        esp_err_t perr = nvs_get_str(h, K_PASS, pass, &l);
        if (perr == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';   // open network — accept missing password
        } else if (perr != ESP_OK) {
            err = perr;
        }
    }
    nvs_close(h);
    if (err == ESP_OK && ssid[0] == '\0') {
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    return err;
}

esp_err_t cfg_save_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved Wi-Fi creds (ssid='%s')", ssid);
    return err;
}

esp_err_t cfg_erase_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // ignore individual erase errors — keys may not exist
    nvs_erase_key(h, K_SSID);
    nvs_erase_key(h, K_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "erased stored Wi-Fi creds");
    return err;
}

bool cfg_has_wifi(void)
{
    char ssid[CFG_FIELD_MAX], pass[CFG_FIELD_MAX];
    return cfg_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;
}

esp_err_t cfg_load_station(char *name, size_t len)
{
    if (!name || len == 0) return ESP_ERR_INVALID_ARG;
    name[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t l = len;
    err = nvs_get_str(h, K_STAT, name, &l);
    nvs_close(h);
    return err;
}

esp_err_t cfg_save_station(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_STAT, name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved station='%s'", name);
    return err;
}

esp_err_t cfg_erase_station(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, K_STAT);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "erased stored station");
    return err;
}

esp_err_t cfg_load_language(char *iso, size_t len)
{
    if (!iso || len == 0) return ESP_ERR_INVALID_ARG;
    iso[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t l = len;
    err = nvs_get_str(h, K_LANG, iso, &l);
    nvs_close(h);
    return err;
}

esp_err_t cfg_save_language(const char *iso)
{
    if (!iso || !iso[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_LANG, iso);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved language='%s'", iso);
    return err;
}

esp_err_t cfg_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);   // nukes every key in this namespace
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "erased the entire 'moshon' NVS namespace");
    return err;
}
