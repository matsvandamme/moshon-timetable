// iRail per-train /vehicle/ fetcher with a small in-memory TTL cache.
//
//   GET https://api.irail.be/v1/vehicle/?id=BE.NMBS.IC1538&date=YYMMDD&format=json
//
// Returns the full route of a single train (all stops). We extract the
// intermediate stop names — i.e. everything between (but not including) the
// origin station the user is watching and the train's final destination —
// and pre-format the LVGL subtitle ("via X, Y, Z").
//
// iRail is rate-limited to 3 req/s + 5 burst per source IP. With ~5 trains
// on a board and a 45 s refresh cycle, the worst case is 5 misses per
// refresh = ~0.11 req/s, well inside budget. The cache TTL means most
// refreshes are pure cache hits.

#include "irail_vehicle.h"
#include "irail.h"          // for IRAIL_VIA_LEN + irail_active_station_id()
#include "app_config.h"
#include "i18n.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

#define IRAIL_VEHICLE_HOST   "https://api.irail.be/v1/vehicle/"
// 64 KB covers international IC trains with very long stops arrays
// (Eurocity / Thalys-replacement trains running through Brussels can
// produce 30-40 KB of JSON each). Lives in PSRAM.
#define IRAIL_VEHICLE_BUF    (64 * 1024)
#define IRAIL_VEHICLE_TO_MS  10000
#define CACHE_SIZE           16
#define CACHE_TTL_SECONDS    300            // 5 min — via lists rarely change

typedef struct {
    char     id[32];
    char     via[IRAIL_VIA_LEN];
    time_t   expires;
} cache_entry_t;

static cache_entry_t   s_cache[CACHE_SIZE];
static SemaphoreHandle_t s_cache_mu = NULL;

static void cache_init_once(void)
{
    if (!s_cache_mu) s_cache_mu = xSemaphoreCreateMutex();
}

// Returns true if cache has a fresh entry; copies the cached via into `out`.
static bool cache_lookup(const char *id, char *out, size_t out_len)
{
    cache_init_once();
    bool hit = false;
    time_t now = time(NULL);
    xSemaphoreTake(s_cache_mu, portMAX_DELAY);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].id[0] && strcmp(s_cache[i].id, id) == 0 &&
            s_cache[i].expires > now) {
            strncpy(out, s_cache[i].via, out_len - 1);
            out[out_len - 1] = '\0';
            hit = true;
            break;
        }
    }
    xSemaphoreGive(s_cache_mu);
    return hit;
}

// Store, evicting the oldest expired entry (or a random slot if all live).
static void cache_store(const char *id, const char *via)
{
    cache_init_once();
    time_t now = time(NULL);
    int victim = 0;
    time_t oldest = INT32_MAX;
    xSemaphoreTake(s_cache_mu, portMAX_DELAY);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].id[0] == '\0' || s_cache[i].expires <= now) {
            victim = i;
            break;
        }
        if (s_cache[i].expires < oldest) {
            oldest = s_cache[i].expires;
            victim = i;
        }
    }
    strncpy(s_cache[victim].id, id, sizeof(s_cache[victim].id) - 1);
    s_cache[victim].id[sizeof(s_cache[victim].id) - 1] = '\0';
    strncpy(s_cache[victim].via, via, sizeof(s_cache[victim].via) - 1);
    s_cache[victim].via[sizeof(s_cache[victim].via) - 1] = '\0';
    s_cache[victim].expires = now + CACHE_TTL_SECONDS;
    xSemaphoreGive(s_cache_mu);
}

// HTTP sink — same pattern as irail.c.
typedef struct { char *buf; size_t cap, len; bool truncated; } http_sink_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_sink_t *sink = (http_sink_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && evt->data_len > 0) {
        size_t avail = sink->cap - sink->len - 1;
        size_t take  = (evt->data_len > (int)avail) ? avail : (size_t)evt->data_len;
        if (take > 0) {
            memcpy(sink->buf + sink->len, evt->data, take);
            sink->len += take;
            sink->buf[sink->len] = '\0';
        }
        if ((size_t)evt->data_len > take) {
            sink->truncated = true;
        }
    }
    return ESP_OK;
}

// Returns true iff iRail marked this stop as already arrived at. Handles
// both number ("arrived": 1) and string ("arrived": "1") encodings that
// have been observed in /vehicle/ responses across the years.
static bool stop_already_arrived(const cJSON *stop)
{
    if (!stop) return false;
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(stop, "arrived");
    if (!j) return false;
    if (cJSON_IsNumber(j))   return j->valueint != 0;
    if (cJSON_IsString(j) && j->valuestring) {
        return j->valuestring[0] == '1';
    }
    return false;
}

// Build "via A, B, C" from the stops array. The two board flavours produce
// different lists:
//
// DEPARTURES (include_terminus == false):
//   Stops that come AFTER our station in the train's direction of travel,
//   excluding the terminus (already shown as the row's yellow label).
//   Stops the train has already called at are never included.
//
// ARRIVALS (include_terminus == true):
//   Every stop the train STILL HAS to call at — starting from the first
//   stop iRail hasn't marked `arrived=1`, through our station, through the
//   stops after our station, ending at the terminus. The user gets the
//   train's remaining future schedule, not just the post-our-station tail.
//
// Matching our station inside the route uses the canonical iRail
// stationinfo.id (language-independent); falls back to a name match when
// no ID is cached. If neither matches in DEPARTURES mode, the result is
// empty — we'd rather show no via than risk leaking pre-our-station
// stops. ARRIVALS mode doesn't need a match because it walks the whole
// not-yet-arrived window.
//
// Long station names that wouldn't fit in out_len drop the partial stop
// and everything after it; no ellipsis is ever written.
static void format_via_from_stops(const cJSON *stops_arr,
                                  const char *origin,
                                  bool include_terminus,
                                  char *out, size_t out_len)
{
    out[0] = '\0';
    if (!cJSON_IsArray(stops_arr) || out_len < 8) return;

    int n = cJSON_GetArraySize(stops_arr);
    if (n < 2) return;   // need at least origin + terminus

    int start = 0;
    int end   = 0;

    if (include_terminus) {
        // Arrivals: start from the first stop the train hasn't visited yet,
        // include everything up to and including the terminus.
        start = 0;
        for (int i = 0; i < n; i++) {
            if (stop_already_arrived(cJSON_GetArrayItem(stops_arr, i))) {
                start = i + 1;
            } else {
                break;
            }
        }
        end = n;
    } else {
        // Departures: find our station, emit only stops after it (exclude
        // the terminus, which is already the row's yellow destination).
        const char *our_id = irail_active_station_id();
        bool have_id = (our_id && our_id[0]);

        int origin_idx = -1;
        for (int i = 0; i < n; i++) {
            const cJSON *stop = cJSON_GetArrayItem(stops_arr, i);
            if (!stop) continue;
            if (have_id) {
                const cJSON *si = cJSON_GetObjectItemCaseSensitive(stop, "stationinfo");
                const cJSON *id_j = cJSON_IsObject(si)
                    ? cJSON_GetObjectItemCaseSensitive(si, "id") : NULL;
                if (cJSON_IsString(id_j) && id_j->valuestring &&
                    strcmp(id_j->valuestring, our_id) == 0) {
                    origin_idx = i;
                    break;
                }
            } else if (origin) {
                const cJSON *station = cJSON_GetObjectItemCaseSensitive(stop, "station");
                if (cJSON_IsString(station) && station->valuestring &&
                    strcmp(station->valuestring, origin) == 0) {
                    origin_idx = i;
                    break;
                }
            }
        }
        if (origin_idx < 0) return;   // strict: never leak pre-our stops

        start = origin_idx + 1;
        end   = n - 1;                // exclude terminus
    }

    if (start >= end) return;

    bool started = false;
    size_t cursor = snprintf(out, out_len, "via ");
    for (int i = start; i < end; i++) {
        const cJSON *stop = cJSON_GetArrayItem(stops_arr, i);
        const cJSON *station = stop ? cJSON_GetObjectItemCaseSensitive(stop, "station") : NULL;
        if (!cJSON_IsString(station) || !station->valuestring) continue;
        const char *name = station->valuestring;

        size_t need = strlen(name) + (started ? 2 : 0);   // 2 = ", "
        if (cursor + need + 1 > out_len) break;

        if (started) {
            memcpy(out + cursor, ", ", 2);
            cursor += 2;
        }
        memcpy(out + cursor, name, strlen(name));
        cursor += strlen(name);
        out[cursor] = '\0';
        started = true;
    }
    if (!started) {
        out[0] = '\0';   // no intermediate stops at all (we may be at terminus)
    }
}

esp_err_t irail_vehicle_get_via(const char *vehicle_id, time_t when,
                                const char *origin, bool include_terminus,
                                char *out_via, size_t out_len)
{
    if (!vehicle_id || !vehicle_id[0] || !out_via || out_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    out_via[0] = '\0';

    // The terminus-included and terminus-excluded variants need separate
    // cache slots — same train can appear in both flavours if the user has
    // both boards loaded back-to-back. Suffix the cache id with "+T" when
    // we're keeping the terminus.
    char cache_id[40];
    snprintf(cache_id, sizeof(cache_id), "%s%s", vehicle_id,
             include_terminus ? "+T" : "");

    if (cache_lookup(cache_id, out_via, out_len)) {
        return ESP_OK;
    }

    char *buf = heap_caps_malloc(IRAIL_VEHICLE_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';
    http_sink_t sink = { .buf = buf, .cap = IRAIL_VEHICLE_BUF, .len = 0, .truncated = false };

    struct tm tm;
    localtime_r(&when, &tm);
    char date_buf[16];
    snprintf(date_buf, sizeof(date_buf), "%02u%02u%02u",
             (unsigned)(tm.tm_mday & 0x3F),
             (unsigned)((tm.tm_mon + 1) & 0x0F),
             (unsigned)(tm.tm_year % 100));

    char url[320];
    snprintf(url, sizeof(url),
             IRAIL_VEHICLE_HOST "?id=%s&date=%s&format=json&lang=%s",
             vehicle_id, date_buf, i18n_iso());

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = IRAIL_VEHICLE_TO_MS,
        .event_handler     = http_event_handler,
        .user_data         = &sink,
        .buffer_size       = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return ESP_FAIL; }
    esp_http_client_set_header(client, "User-Agent", APP_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json");

    ESP_LOGI(TAG_IRAIL, "GET %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG_IRAIL, "vehicle fetch failed: %s status=%d",
                 esp_err_to_name(err), status);
        free(buf);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }
    if (sink.truncated) {
        ESP_LOGW(TAG_IRAIL, "vehicle response exceeded %u-byte buffer (truncated at %u)",
                 (unsigned)IRAIL_VEHICLE_BUF, (unsigned)sink.len);
        free(buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    // iRail's /vehicle/ response has a top-level "stops": { "stop": [...] }.
    cJSON *stops_obj = cJSON_GetObjectItemCaseSensitive(root, "stops");
    cJSON *stops_arr = stops_obj ? cJSON_GetObjectItemCaseSensitive(stops_obj, "stop") : NULL;
    format_via_from_stops(stops_arr, origin, include_terminus, out_via, out_len);

    // Transliterate the assembled string in place — iRail UTF-8 -> ASCII.
    // (Local re-implementation to avoid pulling parser internals in.)
    unsigned char *r = (unsigned char *)out_via;
    unsigned char *w = (unsigned char *)out_via;
    while (*r) {
        if (*r == 0xC3 && r[1]) {
            unsigned char c = r[1];
            char repl = '?';
            // tiny inline lookup for the very common ones
            switch (c) {
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: repl = 'a'; break;
                case 0xA7: repl = 'c'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: repl = 'e'; break;
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: repl = 'i'; break;
                case 0xB1: repl = 'n'; break;
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: repl = 'o'; break;
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: repl = 'u'; break;
                case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: repl = 'A'; break;
                case 0x87: repl = 'C'; break;
                case 0x88: case 0x89: case 0x8A: case 0x8B: repl = 'E'; break;
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: repl = 'I'; break;
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: repl = 'O'; break;
                case 0x99: case 0x9A: case 0x9B: case 0x9C: repl = 'U'; break;
                default:   repl = '?';
            }
            *w++ = (unsigned char)repl;
            r += 2;
        } else if (*r >= 0x80) {
            *w++ = '?'; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';

    cJSON_Delete(root);
    cache_store(cache_id, out_via);
    return ESP_OK;
}
