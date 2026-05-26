#include "irail.h"
#include "app_config.h"
#include "stations.h"
#include "i18n.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"

#include <string.h>
#include <stdlib.h>

// 96 KB is enough headroom for the busiest stations (Brussels-South /
// Bruxelles-Midi typically returns 40-60 KB of JSON per direction). Lives
// in PSRAM so it doesn't eat the precious internal RAM.
#define IRAIL_HTTP_BUF      (96 * 1024)
#define IRAIL_TIMEOUT_MS    10000
#define IRAIL_HOST_PATH     "https://api.irail.be/v1/liveboard/"

// ---------- HTTP collect-to-buffer ----------

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;   // set if the body exceeded `cap` — parser will refuse it
} http_sink_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_sink_t *sink = (http_sink_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (sink && evt->data_len > 0) {
                size_t avail = sink->cap - sink->len - 1;
                size_t take = (evt->data_len > (int)avail) ? avail : (size_t)evt->data_len;
                if (take > 0) {
                    memcpy(sink->buf + sink->len, evt->data, take);
                    sink->len += take;
                    sink->buf[sink->len] = '\0';
                }
                if ((size_t)evt->data_len > take) {
                    sink->truncated = true;
                }
            }
            break;
        default: break;
    }
    return ESP_OK;
}

// ---------- UTF-8 -> ASCII accent transliteration ----------
// LVGL's built-in Montserrat fonts only ship glyphs 0x20-0x7E; anything outside
// that (e.g. "Liège-Guillemins") renders as an empty box. Until we bundle a
// font with extended Latin support, transliterate common Latin-1 accented
// characters (UTF-8 lead byte 0xC2 or 0xC3) to plain ASCII in place.
static void transliterate_utf8(char *s)
{
    if (!s) return;
    static const struct { uint8_t b1, b2; char ascii; } map[] = {
        // 0xC3 ...
        {0xC3,0x80,'A'},{0xC3,0x81,'A'},{0xC3,0x82,'A'},{0xC3,0x83,'A'},
        {0xC3,0x84,'A'},{0xC3,0x85,'A'},{0xC3,0x87,'C'},
        {0xC3,0x88,'E'},{0xC3,0x89,'E'},{0xC3,0x8A,'E'},{0xC3,0x8B,'E'},
        {0xC3,0x8C,'I'},{0xC3,0x8D,'I'},{0xC3,0x8E,'I'},{0xC3,0x8F,'I'},
        {0xC3,0x91,'N'},
        {0xC3,0x92,'O'},{0xC3,0x93,'O'},{0xC3,0x94,'O'},{0xC3,0x95,'O'},{0xC3,0x96,'O'},
        {0xC3,0x99,'U'},{0xC3,0x9A,'U'},{0xC3,0x9B,'U'},{0xC3,0x9C,'U'},
        {0xC3,0xA0,'a'},{0xC3,0xA1,'a'},{0xC3,0xA2,'a'},{0xC3,0xA3,'a'},
        {0xC3,0xA4,'a'},{0xC3,0xA5,'a'},{0xC3,0xA7,'c'},
        {0xC3,0xA8,'e'},{0xC3,0xA9,'e'},{0xC3,0xAA,'e'},{0xC3,0xAB,'e'},
        {0xC3,0xAC,'i'},{0xC3,0xAD,'i'},{0xC3,0xAE,'i'},{0xC3,0xAF,'i'},
        {0xC3,0xB1,'n'},
        {0xC3,0xB2,'o'},{0xC3,0xB3,'o'},{0xC3,0xB4,'o'},{0xC3,0xB5,'o'},{0xC3,0xB6,'o'},
        {0xC3,0xB9,'u'},{0xC3,0xBA,'u'},{0xC3,0xBB,'u'},{0xC3,0xBC,'u'},
        {0xC3,0xBF,'y'},
    };
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;
    while (*r) {
        if (*r == 0xC3 && r[1]) {
            char repl = '?';
            for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
                if (r[0] == map[i].b1 && r[1] == map[i].b2) { repl = map[i].ascii; break; }
            }
            *w++ = (unsigned char)repl;
            r += 2;
        } else if (*r == 0xC2 && r[1]) {
            // Latin-1 control chars / NBSP / etc. -> skip second byte, keep
            // sensible ASCII fallback for the few printable ones.
            *w++ = (r[1] == 0xA0) ? ' ' : '?';
            r += 2;
        } else if (*r >= 0x80) {
            // Other multi-byte sequences we don't handle; drop one byte at a
            // time and emit '?' so we never produce a partial sequence.
            *w++ = '?';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// ---------- Parser ----------

static const char *json_str(const cJSON *obj, const char *key, const char *fallback)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    return fallback;
}

static uint32_t json_uint_from_string_or_number(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return 0;
    if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return (uint32_t)strtoul(v->valuestring, NULL, 10);
    return 0;
}

// iRail wraps fields like "time", "delay", "platform" as strings in JSON.
// "departures": { "number":"N", "departure":[{...},{...}] }
// "arrivals":   { "number":"N", "arrival":[{...},{...}] }
static esp_err_t parse_board(const char *json, bool is_departure, irail_board_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG_IRAIL, "JSON parse error near: %s", cJSON_GetErrorPtr());
        return ESP_FAIL;
    }
    out->count = 0;
    out->fetched_at = time(NULL);

    cJSON *section = cJSON_GetObjectItemCaseSensitive(root, is_departure ? "departures" : "arrivals");
    cJSON *arr     = section ? cJSON_GetObjectItemCaseSensitive(section,
                                  is_departure ? "departure" : "arrival") : NULL;
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG_IRAIL, "No %s array in response", is_departure ? "departures" : "arrivals");
        cJSON_Delete(root);
        return ESP_OK;  // empty board is still a successful fetch
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (out->count >= IRAIL_MAX_ENTRIES) break;
        irail_entry_t *e = &out->entries[out->count];
        memset(e, 0, sizeof(*e));

        e->scheduled     = (time_t)json_uint_from_string_or_number(item, "time");
        e->delay_seconds = json_uint_from_string_or_number(item, "delay");
        e->canceled      = json_uint_from_string_or_number(item, "canceled") != 0;
        e->left_or_arrived = json_uint_from_string_or_number(item, is_departure ? "left" : "arrived") != 0;

        const cJSON *station = cJSON_GetObjectItemCaseSensitive(item, "station");
        if (cJSON_IsString(station) && station->valuestring) {
            strncpy(e->other_station, station->valuestring, IRAIL_FIELD_LEN - 1);
            transliterate_utf8(e->other_station);
        }

        const cJSON *vehicle_info = cJSON_GetObjectItemCaseSensitive(item, "vehicleinfo");
        const char *short_name = NULL;
        const char *type_str   = NULL;
        const char *name_str   = NULL;   // canonical id like "BE.NMBS.IC1538"
        if (cJSON_IsObject(vehicle_info)) {
            short_name = json_str(vehicle_info, "shortname", NULL);
            type_str   = json_str(vehicle_info, "type", NULL);
            name_str   = json_str(vehicle_info, "name", NULL);
        }
        if (!short_name) short_name = json_str(item, "vehicle", "?");
        strncpy(e->vehicle, short_name, IRAIL_FIELD_LEN - 1);
        transliterate_utf8(e->vehicle);
        if (name_str && name_str[0]) {
            strncpy(e->vehicle_id, name_str, IRAIL_VEHID_LEN - 1);
        }

        // Prefer explicit `type` field from vehicleinfo; else split shortname
        // before the first space (e.g. "IC 1538" -> "IC", "S81 7234" -> "S81").
        if (type_str && type_str[0]) {
            strncpy(e->type, type_str, sizeof(e->type) - 1);
        } else {
            const char *space = strchr(short_name, ' ');
            size_t type_len = space ? (size_t)(space - short_name) : strlen(short_name);
            if (type_len > sizeof(e->type) - 1) type_len = sizeof(e->type) - 1;
            memcpy(e->type, short_name, type_len);
            e->type[type_len] = '\0';
        }

        const char *plat = json_str(item, "platform", "?");
        strncpy(e->platform, plat, sizeof(e->platform) - 1);

        out->count++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// ---------- One liveboard fetch ----------

static esp_err_t fetch(const char *arrdep, time_t for_time, irail_board_t *out)
{
    // Allocate in PSRAM — 96 KB would eat too much of the ~512 KB internal
    // RAM that's already shared with code, stacks and Wi-Fi/lwIP/mbedtls.
    char *buf = heap_caps_malloc(IRAIL_HTTP_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';
    http_sink_t sink = { .buf = buf, .cap = IRAIL_HTTP_BUF, .len = 0, .truncated = false };

    // iRail accepts station= with the canonical name (any of nl/fr/en/de).
    // It also handles URL-encoding of accents like "Liège-Guillemins" for us
    // here via esp_http_client's URL parsing.
    const station_t *st = station_get_active();
    char url[320];
    if (for_time > 0) {
        // Specific date/time fetch. iRail wants date=ddmmyy & time=hhmm in
        // local (Brussels) time; we already have TZ=CET via time_sync_start.
        struct tm tm;
        localtime_r(&for_time, &tm);
        // GCC's -Werror=format-truncation can't prove tm_* fields are bounded,
        // so use generously-sized buffers and clamped unsigneds.
        char date_buf[16], time_buf[16];
        snprintf(date_buf, sizeof(date_buf), "%02u%02u%02u",
                 (unsigned)(tm.tm_mday & 0x3F),
                 (unsigned)((tm.tm_mon + 1) & 0x0F),
                 (unsigned)(tm.tm_year % 100));
        snprintf(time_buf, sizeof(time_buf), "%02u%02u",
                 (unsigned)(tm.tm_hour & 0x3F),
                 (unsigned)(tm.tm_min  & 0x3F));
        snprintf(url, sizeof(url),
                 IRAIL_HOST_PATH "?station=%s&arrdep=%s&format=json&lang=%s"
                 "&alerts=false&date=%s&time=%s",
                 st->query_name, arrdep, i18n_iso(), date_buf, time_buf);
    } else {
        snprintf(url, sizeof(url),
                 IRAIL_HOST_PATH "?station=%s&arrdep=%s&format=json&lang=%s&alerts=false",
                 st->query_name, arrdep, i18n_iso());
    }

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = IRAIL_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &sink,
        .buffer_size       = 4096,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(buf);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", APP_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json");

    ESP_LOGI(TAG_IRAIL, "GET %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_IRAIL, "HTTP error: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG_IRAIL, "HTTP status %d", status);
        free(buf);
        return ESP_FAIL;
    }
    if (sink.truncated) {
        // Truncated JSON would fail cJSON_Parse anyway; bail loudly so the
        // user sees a real diagnostic in the serial log instead of a silent
        // stuck-on-splash state. Bump IRAIL_HTTP_BUF if this fires.
        ESP_LOGE(TAG_IRAIL, "Response exceeded %u-byte buffer (truncated at %u)",
                 (unsigned)IRAIL_HTTP_BUF, (unsigned)sink.len);
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_IRAIL, "Body %u bytes", (unsigned)sink.len);
    esp_err_t prc = parse_board(buf, strcmp(arrdep, "departure") == 0, out);
    free(buf);
    if (prc == ESP_OK) {
        out->for_time = for_time;
        ESP_LOGI(TAG_IRAIL, "%s: parsed %u entries%s",
                 arrdep, (unsigned)out->count,
                 for_time > 0 ? " (scheduled fetch)" : "");
    }
    return prc;
}

esp_err_t irail_fetch(const char *arrdep, time_t for_time, irail_board_t *out)
{ return fetch(arrdep, for_time, out); }

esp_err_t irail_fetch_departures(irail_board_t *out) { return fetch("departure", 0, out); }
esp_err_t irail_fetch_arrivals  (irail_board_t *out) { return fetch("arrival",   0, out); }
