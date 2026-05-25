#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up Wi-Fi station mode with credentials from wifi_secrets.h.
// Blocks until either:
//   - associated + got IP (returns ESP_OK), or
//   - timeout_ms elapsed (returns ESP_ERR_TIMEOUT).
// Safe to call once at boot; reconnection is handled automatically afterwards.
esp_err_t wifi_start_and_wait(uint32_t timeout_ms);

// True if currently connected with a valid IP.
bool wifi_is_connected(void);

// Start SNTP sync (call after wifi_start_and_wait). Sets Europe/Brussels TZ.
esp_err_t time_sync_start(void);

#ifdef __cplusplus
}
#endif
