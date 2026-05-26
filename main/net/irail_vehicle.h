#pragma once

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Look up the formatted "via X, Y, Z" subtitle for a train, hitting iRail's
// /vehicle/?id=... endpoint if the entry isn't already in our in-memory
// cache. Blocks for up to a few seconds on a cache miss (TLS handshake +
// JSON parse). On any error, leaves `out_via` as an empty string and
// returns the error.
//
//   vehicle_id        : canonical iRail id, e.g. "BE.NMBS.IC1538"
//   when              : scheduled time of the train (used for date in the query)
//   origin            : the station whose liveboard this came from. Only stops
//                       AFTER this station in the direction of travel are
//                       included; the user's own station is pruned.
//   include_terminus  : if true (use for ARRIVAL boards) the final destination
//                       is appended as the last via stop. If false (DEPARTURE
//                       boards) it is omitted — it's already shown in yellow.
//   out_via           : caller buffer; receives "via Halle, Brussels-South" or "".
//   out_len           : capacity of out_via (e.g. IRAIL_VIA_LEN).
esp_err_t irail_vehicle_get_via(const char *vehicle_id,
                                time_t      when,
                                const char *origin,
                                bool        include_terminus,
                                char       *out_via,
                                size_t      out_len);

#ifdef __cplusplus
}
#endif
