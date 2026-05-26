#pragma once

#include "irail.h"

#ifdef __cplusplus
extern "C" {
#endif

// Populate `out` with synthetic, realistic-looking departures from Aalter,
// anchored at `now` so the times always look "current". Includes a couple of
// delays, a cancellation, accented names, and a long destination so the UI
// can be eyeballed without poking iRail.
void demo_fill_departures(irail_board_t *out, time_t now);

// Same idea for arrivals.
void demo_fill_arrivals(irail_board_t *out, time_t now);

#ifdef __cplusplus
}
#endif
