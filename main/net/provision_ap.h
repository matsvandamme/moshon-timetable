#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROVISION_AP_SSID "moshon-setup"
#define PROVISION_AP_URL  "http://192.168.4.1"

// Bring up an open SoftAP "moshon-setup" on 192.168.4.1, start a tiny
// HTTP server that serves a setup form at any URL (captive-portal style),
// and a DNS responder that answers every query with 192.168.4.1 so phones
// auto-detect the captive portal and open the page.
//
// On form submit, the new credentials + station are written to NVS and
// the chip is restarted. This function returns ESP_OK after the AP is up;
// it does NOT block — it spawns its own tasks for DNS and httpd.
esp_err_t provision_ap_start(void);

#ifdef __cplusplus
}
#endif
