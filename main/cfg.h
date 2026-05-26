#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum length of any single config string (SSID, password, station name).
#define CFG_FIELD_MAX  64

// One-time init. Call after nvs_flash_init() succeeds.
esp_err_t cfg_init(void);

// ---------- Wi-Fi credentials ----------

// Load SSID and password from NVS. Returns ESP_OK only when both buffers
// are populated AND SSID is non-empty. Password may be empty for open APs.
esp_err_t cfg_load_wifi(char *ssid, size_t ssid_len,
                        char *pass, size_t pass_len);

// Save SSID + password (overwriting any existing values). Empty password
// is allowed (e.g. for open networks).
esp_err_t cfg_save_wifi(const char *ssid, const char *pass);

// Wipe the stored credentials. Used by the on-screen long-press to force
// the device back into AP provisioning mode on next boot.
esp_err_t cfg_erase_wifi(void);

// Quick "do we have usable credentials" check used by the boot branch.
bool      cfg_has_wifi(void);

// ---------- Station selection ----------

// Load the user-selected station name (as the iRail query expects it,
// e.g. "Brussels-South"). Returns ESP_ERR_NVS_NOT_FOUND if never set.
esp_err_t cfg_load_station(char *name, size_t len);
esp_err_t cfg_save_station(const char *name);
esp_err_t cfg_erase_station(void);

// ---------- UI language ----------

// ISO-639-1 two-letter code: "nl", "fr", "en" or "de". Loaded at boot
// by i18n_init(); written by the captive-portal /save handler.
esp_err_t cfg_load_language(char *iso, size_t len);
esp_err_t cfg_save_language(const char *iso);

// ---------- Night-time dimming ----------

// Display is fully off between dim_start_hour (inclusive) and
// dim_end_hour (exclusive), in local time (Europe/Brussels). Defaults
// to off-off (both 0), meaning the display is always on.
// Hours are 0..23. Saving start == end disables the dim window.
esp_err_t cfg_load_dim_hours(uint8_t *start_hour, uint8_t *end_hour);
esp_err_t cfg_save_dim_hours(uint8_t start_hour, uint8_t end_hour);

// ---------- Weather ----------

// Optional latitude / longitude for the Open-Meteo header widget.
// If neither is saved, the header just doesn't show weather.
esp_err_t cfg_load_weather(float *lat, float *lon);
esp_err_t cfg_save_weather(float lat, float lon);

// ---------- Wipe everything ----------

// Convenience: erase Wi-Fi creds AND station. Used by the on-screen
// long-press reset so the next boot lands cleanly in AP provisioning.
esp_err_t cfg_erase_all(void);

#ifdef __cplusplus
}
#endif
