#include "irail.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"

#include <string.h>
#include <stdlib.h>

#define IRAIL_HTTP_BUF      (16 * 1024)
#define IRAIL_TIMEOUT_MS    10000
#define IRAIL_HOST_PATH     "https://api.irail.be/v1/liveboard/"

// ---------- HTTP collect-to-buffer ----------

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
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
            }
            break;
        default: break;
    }
    return ESP_OK;
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
        }

        const cJSON *vehicle_info = cJSON_GetObjectItemCaseSensitive(item, "vehicleinfo");
        const char *short_name = NULL;
        if (cJSON_IsObject(vehicle_info)) {
            short_name = json_str(vehicle_info, "shortname", NULL);
        }
        if (!short_name) short_name = json_str(item, "vehicle", "?");
        strncpy(e->vehicle, short_name, IRAIL_FIELD_LEN - 1);

        const char *plat = json_str(item, "platform", "?");
        strncpy(e->platform, plat, sizeof(e->platform) - 1);

        out->count++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// ---------- One liveboard fetch ----------

static esp_err_t fetch(const char *arrdep, irail_board_t *out)
{
    char *buf = malloc(IRAIL_HTTP_BUF);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';
    http_sink_t sink = { .buf = buf, .cap = IRAIL_HTTP_BUF, .len = 0 };

    char url[256];
    snprintf(url, sizeof(url),
             IRAIL_HOST_PATH "?id=%s&arrdep=%s&format=json&lang=en&alerts=false",
             STATION_ID, arrdep);

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

    ESP_LOGD(TAG_IRAIL, "Body %u bytes", (unsigned)sink.len);
    esp_err_t prc = parse_board(buf, strcmp(arrdep, "departure") == 0, out);
    free(buf);
    return prc;
}

esp_err_t irail_fetch_departures(irail_board_t *out) { return fetch("departure", out); }
esp_err_t irail_fetch_arrivals  (irail_board_t *out) { return fetch("arrival",   out); }
