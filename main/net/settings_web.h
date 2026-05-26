#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the on-network settings page + OTA endpoint + mDNS hostname.
// Call once after Wi-Fi STA has associated and an IP is up.
//
// Once running, the device is reachable at:
//   http://moshon-timetable.local/        (settings page, current values pre-filled)
//   http://moshon-timetable.local/save    (POST: persist + reboot)
//   http://moshon-timetable.local/ota     (POST: pull latest GitHub release)
//
// The settings page exposes the same fields as the captive-portal setup
// flow (language, Wi-Fi, station) plus night-time dim hours and weather
// coordinates. Saving from here behaves the same as the captive portal:
// values are persisted to NVS and the device reboots to pick them up.
esp_err_t settings_web_start(void);

// Tear down the HTTP server + mDNS records. Currently only used in tests.
esp_err_t settings_web_stop(void);

#ifdef __cplusplus
}
#endif
