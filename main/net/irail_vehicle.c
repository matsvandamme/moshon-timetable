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
#include "irail.h"          // for IRAIL_VIA_LEN
#include "app_config.h"
#include "i18n.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

#define IRAIL_VEHICLE_HOST   "https://api.irail.be/v1/vehicle/"
#define IRAIL_VEHICLE_BUF    (24 * 1024)    // /vehicle/ responses are larger
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
typedef struct { char *buf; size_t cap, len; } http_sink_t;

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
    }
    return ESP_OK;
}

// Build "via A, B, C" from the stops array, showing ONLY the stops that
// come AFTER the timetable station in the train's direction of travel.
// The final destination (terminus) is excluded — it's already shown as
// the row's primary destination label.
//
// This is correct for both boards:
//   - Departures: the user is at `origin`; they want to know which calls
//     come between here and the terminus.
//   - Arrivals:   the train arrives at `origin`; the user might transfer
//     onward, so they want the calls AFTER our station, not the ones the
//     train has already left behind.
//
// If a stop name wouldn't fit fully in out_len it (and anything after it)
// is silently dropped — we NEVER append "..." or any other ellipsis
// marker, so the UI is guaranteed to only ever show full station names.
// The on-screen page-flip in ui.c already handles long via lists by
// paging through them.
static void format_via_from_stops(const cJSON *stops_arr,
                                  const char *origin,
                                  char *out, size_t out_len)
{
    out[0] = '\0';
    if (!cJSON_IsArray(stops_arr) || out_len < 8) return;

    int n = cJSON_GetArraySize(stops_arr);
    if (n < 3) return;   // origin + terminus only -> no via stops

    // Find the timetable station's index in the route. If iRail's station
    // names don't match (e.g. when the user picked a non-Dutch UI but the
    // saved station was selected from the Dutch picker), origin_idx stays
    // -1 and we fall back to "skip stops named like origin" so the result
    // is degraded but never wrong.
    int origin_idx = -1;
    if (origin) {
        for (int i = 0; i < n; i++) {
            const cJSON *stop = cJSON_GetArrayItem(stops_arr, i);
            const cJSON *station = stop ? cJSON_GetObjectItemCaseSensitive(stop, "station") : NULL;
            if (cJSON_IsString(station) && station->valuestring &&
                strcmp(station->valuestring, origin) == 0) {
                origin_idx = i;
                break;
            }
        }
    }

    // Start from one past our station (if found) or from the very first
    // stop (if not). Stop one short of the terminus.
    int start = (origin_idx >= 0) ? origin_idx + 1 : 0;
    int end   = n - 1;   // exclusive (= skip terminus)

    bool started = false;
    size_t cursor = snprintf(out, out_len, "via ");
    for (int i = start; i < end; i++) {
        const cJSON *stop = cJSON_GetArrayItem(stops_arr, i);
        const cJSON *station = stop ? cJSON_GetObjectItemCaseSensitive(stop, "station") : NULL;
        if (!cJSON_IsString(station) || !station->valuestring) continue;
        const char *name = station->valuestring;
        // Fallback path: also defend against the origin name appearing
        // in our window when we couldn't pin its index.
        if (origin_idx < 0 && origin && strcmp(name, origin) == 0) continue;

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
                                const char *origin,
                                char *out_via, size_t out_len)
{
    if (!vehicle_id || !vehicle_id[0] || !out_via || out_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    out_via[0] = '\0';

    if (cache_lookup(vehicle_id, out_via, out_len)) {
        return ESP_OK;
    }

    char *buf = malloc(IRAIL_VEHICLE_BUF);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';
    http_sink_t sink = { .buf = buf, .cap = IRAIL_VEHICLE_BUF, .len = 0 };

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

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    // iRail's /vehicle/ response has a top-level "stops": { "stop": [...] }.
    cJSON *stops_obj = cJSON_GetObjectItemCaseSensitive(root, "stops");
    cJSON *stops_arr = stops_obj ? cJSON_GetObjectItemCaseSensitive(stops_obj, "stop") : NULL;
    format_via_from_stops(stops_arr, origin, out_via, out_len);

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
    cache_store(vehicle_id, out_via);
    return ESP_OK;
}
