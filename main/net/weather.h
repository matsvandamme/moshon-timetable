#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pull current weather from Open-Meteo (free, no API key) for the
// configured coordinates. Result is cached internally for a few minutes.
// On success, `temp_c` is filled with the current air temperature in
// degrees Celsius and `weather_code` with the WMO weather interpretation
// code (0 = clear sky, 1..3 = partly cloudy, 45/48 = fog, etc.).
// Returns ESP_ERR_NOT_FOUND if no coordinates are configured in NVS.
esp_err_t weather_fetch(float *temp_c, int *weather_code);

// True if NVS has weather coordinates configured. Cheap check that
// avoids the network call. Used by the UI to decide whether to allocate
// the header label.
bool weather_enabled(void);

#ifdef __cplusplus
}
#endif
