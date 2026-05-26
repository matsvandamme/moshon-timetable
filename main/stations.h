#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Human-readable label shown on the LCD header.
    const char *display_name;
    // iRail `station=` query value. See https://api.irail.be/stations/.
    // Pass URL-encoded if the name contains accents or spaces — iRail
    // handles the lookup either way for canonical names.
    const char *query_name;
} station_t;

// Return the station selected via Kconfig (menu "Moshon Timetable").
// Never NULL — falls back to Aalter if no choice is set.
const station_t *station_get_active(void);

#ifdef __cplusplus
}
#endif
