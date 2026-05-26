// Open-Meteo current-weather fetcher with a small in-RAM cache.
//
// Open-Meteo is a free, no-API-key weather service. Its `current=`
// endpoint returns just a few fields and a tiny payload (~250 bytes), so
// the parse is hand-rolled — no cJSON dependency at this site.

#include "weather.h"
#include "cfg.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "weather";

#define BUF_CAP        1024
#define CACHE_TTL_SEC  600   // 10 min — weather doesn't change fast enough to spam Open-Meteo

static float  s_cached_temp     = 0.0f;
static int    s_cached_code     = -1;
static time_t s_cached_at       = 0;

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    sink_t *s = (sink_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && s && evt->data_len > 0) {
        size_t avail = s->cap - s->len - 1;
        size_t take  = (evt->data_len > (int)avail) ? avail : (size_t)evt->data_len;
        if (take > 0) {
            memcpy(s->buf + s->len, evt->data, take);
            s->len += take;
            s->buf[s->len] = '\0';
        }
    }
    return ESP_OK;
}

// Find a key like "temperature_2m":-3.4 in the JSON body and parse the
// number that follows. Returns true on success.
static bool parse_number_after(const char *body, const char *key, double *out)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

bool weather_enabled(void)
{
    float lat, lon;
    return cfg_load_weather(&lat, &lon) == ESP_OK
        && (lat != 0.0f || lon != 0.0f);
}

esp_err_t weather_fetch(float *temp_c, int *weather_code)
{
    if (!temp_c || !weather_code) return ESP_ERR_INVALID_ARG;

    float lat = 0.0f, lon = 0.0f;
    if (cfg_load_weather(&lat, &lon) != ESP_OK ||
        (lat == 0.0f && lon == 0.0f)) {
        return ESP_ERR_NOT_FOUND;
    }

    time_t now = time(NULL);
    if (s_cached_code >= 0 && (now - s_cached_at) < CACHE_TTL_SEC) {
        *temp_c = s_cached_temp;
        *weather_code = s_cached_code;
        return ESP_OK;
    }

    char *buf = malloc(BUF_CAP);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';
    sink_t sink = { .buf = buf, .cap = BUF_CAP, .len = 0 };

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code&timezone=Europe%%2FBrussels",
        lat, lon);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = http_event,
        .user_data = &sink,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return ESP_FAIL; }
    esp_http_client_set_header(client, "User-Agent", APP_USER_AGENT);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch failed: err=%s status=%d", esp_err_to_name(err), status);
        free(buf);
        return ESP_FAIL;
    }

    double t = 0.0, code = -1.0;
    bool ok_t = parse_number_after(buf, "temperature_2m", &t);
    bool ok_c = parse_number_after(buf, "weather_code", &code);
    free(buf);

    if (!ok_t || !ok_c) {
        ESP_LOGW(TAG, "could not parse current weather (temp=%d code=%d)", ok_t, ok_c);
        return ESP_FAIL;
    }

    s_cached_temp = (float)t;
    s_cached_code = (int)code;
    s_cached_at   = now;
    *temp_c       = s_cached_temp;
    *weather_code = s_cached_code;
    ESP_LOGI(TAG, "weather: %.1f C, code=%d", *temp_c, *weather_code);
    return ESP_OK;
}
