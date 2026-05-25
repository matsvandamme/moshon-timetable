#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRAIL_MAX_ENTRIES   12
#define IRAIL_FIELD_LEN     48

typedef struct {
    time_t   scheduled;            // absolute time (unix)
    uint32_t delay_seconds;
    char     other_station[IRAIL_FIELD_LEN]; // destination for departures, origin for arrivals
    char     vehicle[IRAIL_FIELD_LEN];       // e.g. "IC 1538"
    char     platform[8];                    // "1", "2A", "?"
    bool     canceled;
    bool     left_or_arrived;
} irail_entry_t;

typedef struct {
    irail_entry_t entries[IRAIL_MAX_ENTRIES];
    size_t        count;
    time_t        fetched_at;
} irail_board_t;

// Fetch the next departures from STATION_NAME (defined in app_config.h).
esp_err_t irail_fetch_departures(irail_board_t *out);

// Fetch the next arrivals at STATION_NAME.
esp_err_t irail_fetch_arrivals(irail_board_t *out);

#ifdef __cplusplus
}
#endif
