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
#define IRAIL_VEHID_LEN     32     // e.g. "BE.NMBS.IC2803"
#define IRAIL_VIA_LEN       256    // pre-formatted "via X, Y, Z" subtitle —
                                   // generous so late stops aren't dropped at
                                   // the source (the on-screen paginator in
                                   // ui.c handles wide via lines visually)
#define IRAIL_ALERT_LEN     192    // one-line alert headline from liveboard

typedef struct {
    time_t   scheduled;                      // absolute time (unix)
    uint32_t delay_seconds;
    char     other_station[IRAIL_FIELD_LEN]; // destination for departures, origin for arrivals
    char     vehicle[IRAIL_FIELD_LEN];       // full shortname, e.g. "IC 1538"
    char     type[8];                        // class only, e.g. "IC", "S8", "P", "L"
    char     platform[8];                    // "1", "2A", "?"
    char     vehicle_id[IRAIL_VEHID_LEN];    // for /vehicle/?id=... lookup
    char     via[IRAIL_VIA_LEN];             // "via X, Y, Z" (filled async)
    bool     canceled;
    bool     left_or_arrived;
} irail_entry_t;

typedef struct {
    irail_entry_t entries[IRAIL_MAX_ENTRIES];
    size_t        count;
    time_t        fetched_at;
    time_t        for_time;   // 0 = "now"; else the absolute unix time we
                              // asked iRail for (used by the overnight
                              // fallback that requests tomorrow ~05:00).
    char          alert_headline[IRAIL_ALERT_LEN];  // empty when no active
                                                    // service alert
} irail_board_t;

// Fetch the liveboard for the active station at a specific moment in time.
// `for_time = 0` means "now" (iRail's default). `arrdep` is "departure" or
// "arrival". The returned board's `for_time` field mirrors what was asked.
esp_err_t irail_fetch(const char *arrdep, time_t for_time, irail_board_t *out);

// Backwards-compat thin wrappers — both call irail_fetch(..., 0, ...).
esp_err_t irail_fetch_departures(irail_board_t *out);
esp_err_t irail_fetch_arrivals(irail_board_t *out);

// Latest iRail station ID (e.g. "BE.NMBS.008892007") for the queried
// station. Populated as a side effect of irail_fetch when the response
// contains a stationinfo block. Returns "" until the first successful
// fetch. Used by irail_vehicle.c to match the user's station in the
// /vehicle/ stops array independent of UI language — station ID is
// canonical, station name is not.
const char *irail_active_station_id(void);

#ifdef __cplusplus
}
#endif
