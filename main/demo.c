// Synthetic boards for demo mode (CONFIG_DEMO_MODE=y). Lets you see how the
// UI renders delays, cancellations, accents, and long destinations without
// waiting for real iRail data — handy for screenshots and showing the device
// off when nothing is actually running at the station.

#include "demo.h"
#include <string.h>
#include <stdio.h>

static void set_entry(irail_entry_t *e,
                      time_t t,
                      uint32_t delay_seconds,
                      const char *other_station,
                      const char *vehicle,
                      const char *type,
                      const char *platform,
                      const char *via,
                      bool canceled)
{
    memset(e, 0, sizeof(*e));
    e->scheduled     = t;
    e->delay_seconds = delay_seconds;
    strncpy(e->other_station, other_station, IRAIL_FIELD_LEN - 1);
    strncpy(e->vehicle,       vehicle,       IRAIL_FIELD_LEN - 1);
    strncpy(e->type,          type,          sizeof(e->type) - 1);
    strncpy(e->platform,      platform,      sizeof(e->platform) - 1);
    if (via) strncpy(e->via,  via,           IRAIL_VIA_LEN - 1);
    e->canceled = canceled;
}

void demo_fill_departures(irail_board_t *out, time_t now)
{
    memset(out, 0, sizeof(*out));
    out->fetched_at = now;

    // Round "now" up to the next minute boundary so the times look clean.
    time_t base = now - (now % 60);

    // 1. On-time IC. Long via list intentionally — should trigger marquee scroll.
    set_entry(&out->entries[0],
              base +  3*60, 0,
              "Quevry", "IC 2804", "IC", "19",
              "via Halle, 's-Gravenbrakel, La Louviere-Zuid, Mons",
              false);

    // 2. Six-minute delay, medium via.
    set_entry(&out->entries[1],
              base +  3*60, 6*60,
              "Gent-Sint-Pieters", "IC 1538", "IC", "7",
              "via Denderleeuw, Aalst, Wetteren, Schellebelle",
              false);

    // 3. Massive 90-minute delay, short via (no marquee here so contrast).
    set_entry(&out->entries[2],
              base +  3*60, 90*60,
              "Antwerpen-Centraal", "IC 540", "IC", "13",
              "via Mechelen",
              false);

    // 4. Cancellation — strikethrough destination + red AFG badge.
    set_entry(&out->entries[3],
              base + 11*60, 0,
              "Louvain-la-Neuve", "S8 7234", "S8", "18",
              "via Brussel-Schuman, Etterbeek, Watermael, Boitsfort",
              true);

    // 5. International destination, on time. Another long via -> marquee.
    set_entry(&out->entries[4],
              base + 11*60, 0,
              "Dortmund Hbf", "ICE 213", "ICE", "3",
              "via Liege-Guillemins, Aachen, Koln, Dusseldorf, Essen",
              false);

    out->count = 5;
}

void demo_fill_arrivals(irail_board_t *out, time_t now)
{
    memset(out, 0, sizeof(*out));
    out->fetched_at = now;
    time_t base = now - (now % 60);

    // 1. Brugge arrival, on time, no via (regional)
    set_entry(&out->entries[0],
              base +  2*60, 0,
              "Brugge", "IC 1505", "IC", "1",
              "via Beernem",
              false);

    // 2. Slight delay
    set_entry(&out->entries[1],
              base +  4*60, 5*60,
              "Kortrijk", "L 122", "L", "2",
              "via Tielt, Deinze",
              false);

    // 3. Oostende, on time, long via
    set_entry(&out->entries[2],
              base +  9*60, 0,
              "Oostende", "IC 1838", "IC", "1",
              "via De Pinte, Brugge",
              false);

    // 4. International arrival, cancelled
    set_entry(&out->entries[3],
              base + 14*60, 0,
              "Lille-Flandres", "TGV 9876", "TGV", "4",
              "via Mouscron",
              true);

    // 5. Local with a noticeable delay
    set_entry(&out->entries[4],
              base + 18*60, 12*60,
              "Tournai", "L 555", "L", "2",
              "via Oudenaarde, Ronse",
              false);

    out->count = 5;
}
