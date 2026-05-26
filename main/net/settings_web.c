// On-network settings + OTA web UI.
//
// Runs alongside the live timetable once STA has an IP. Shares the same
// HTML look as the captive portal but pre-fills every field with the
// values currently saved in NVS so the user only has to change what they
// want to change. The /ota endpoint pulls the newest release asset from
// GitHub and lets esp_https_ota flash it into the inactive partition.
//
// Discovery is via mDNS: the device advertises itself as
// `moshon-timetable.local` over Bonjour / Avahi.

#include "settings_web.h"
#include "cfg.h"
#include "i18n.h"
#include "stations.h"
#include "stations_be.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "settings";
static httpd_handle_t s_httpd = NULL;

// ----------------------------------------------------------------------
// HTML form (same look as captive portal). Streamed in three chunks so
// the 25 KB stations datalist doesn't have to live in one huge string.
// ----------------------------------------------------------------------

static const char HTML_HEAD[] =
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>moshon-timetable</title>"
    "<style>"
    "html,body{margin:0;padding:0}"
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
    "background:#0A1F44;color:#fff;padding:20px;max-width:520px;margin:0 auto;"
    "min-height:100vh;box-sizing:border-box}"
    "h1{margin:0 0 6px;color:#FCD500;font-size:22px}"
    "h2{margin:24px 0 8px;color:#FCD500;font-size:15px;text-transform:uppercase;"
    "letter-spacing:1px;border-bottom:1px solid #FCD50033;padding-bottom:4px}"
    "p.sub{margin:0 0 22px;color:#aab;font-size:14px}"
    "form{display:flex;flex-direction:column;gap:14px}"
    "label{display:flex;flex-direction:column;gap:4px;font-size:13px;"
    "color:#aab;text-transform:uppercase;letter-spacing:.5px}"
    "input,select{padding:11px 12px;font-size:16px;border:1px solid #fff3;"
    "border-radius:8px;background:#002F6C;color:#fff;box-sizing:border-box}"
    "input:focus,select:focus{outline:2px solid #FCD500;border-color:transparent}"
    ".row{display:flex;gap:10px}.row label{flex:1}"
    "button{margin-top:8px;padding:14px;background:#FCD500;color:#002F6C;"
    "border:0;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}"
    "button.secondary{background:#33415c;color:#fff}"
    "button:active{opacity:.85}"
    ".foot{margin-top:30px;font-size:11px;color:#67c;text-align:center}"
    ".note{font-size:12px;color:#88a;margin-top:-6px}"
    "</style></head><body>"
    "<h1>moshon-timetable</h1>"
    "<p class=\"sub\">Change settings without losing your Wi-Fi credentials.</p>";

static const char HTML_AFTER_STATIONS[] = "</datalist></body></html>";

// ----------------------------------------------------------------------
// URL-decode helper (shared logic with provision_ap, kept local to avoid
// pulling its private header out).
// ----------------------------------------------------------------------
static void url_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else { *w++ = *r++; }
    }
    *w = '\0';
}

static void html_escape_chunk(httpd_req_t *req, const char *s)
{
    // Stream the string, escaping the four HTML-attribute-dangerous
    // characters. Keeps prefilled values from breaking the layout if a
    // saved SSID happens to contain " or & etc.
    const char *p = s;
    const char *q = s;
    while (*q) {
        const char *repl = NULL;
        switch (*q) {
            case '<': repl = "&lt;";   break;
            case '>': repl = "&gt;";   break;
            case '&': repl = "&amp;";  break;
            case '"': repl = "&quot;"; break;
            default:  repl = NULL;
        }
        if (repl) {
            if (q > p) httpd_resp_send_chunk(req, p, q - p);
            httpd_resp_send_chunk(req, repl, HTTPD_RESP_USE_STRLEN);
            p = q + 1;
        }
        q++;
    }
    if (q > p) httpd_resp_send_chunk(req, p, q - p);
}

// ----------------------------------------------------------------------
// GET / — render the settings form with current values
// ----------------------------------------------------------------------
static esp_err_t serve_form(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char ssid[CFG_FIELD_MAX]    = {0};
    char pass[CFG_FIELD_MAX]    = {0};
    char station[CFG_FIELD_MAX] = {0};
    char lang[8]                = {0};
    uint8_t dim_s = 0, dim_e = 0;
    float wx_lat = 0.0f, wx_lon = 0.0f;

    cfg_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    cfg_load_station(station, sizeof(station));
    cfg_load_language(lang, sizeof(lang));
    cfg_load_dim_hours(&dim_s, &dim_e);
    bool have_wx = (cfg_load_weather(&wx_lat, &wx_lon) == ESP_OK);

    httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<h2>Display</h2><form method=\"POST\" action=\"/save\">",
                          HTTPD_RESP_USE_STRLEN);

    // Language select with current language pre-selected
    httpd_resp_send_chunk(req,
        "<label>Display language<select name=\"lang\" required>"
        "<option value=\"nl\"", HTTPD_RESP_USE_STRLEN);
    if (strcmp(lang, "nl") == 0) httpd_resp_send_chunk(req, " selected", 9);
    httpd_resp_send_chunk(req, ">Nederlands</option><option value=\"fr\"", HTTPD_RESP_USE_STRLEN);
    if (strcmp(lang, "fr") == 0) httpd_resp_send_chunk(req, " selected", 9);
    httpd_resp_send_chunk(req, ">Francais</option><option value=\"en\"", HTTPD_RESP_USE_STRLEN);
    if (strcmp(lang, "en") == 0) httpd_resp_send_chunk(req, " selected", 9);
    httpd_resp_send_chunk(req, ">English</option><option value=\"de\"", HTTPD_RESP_USE_STRLEN);
    if (strcmp(lang, "de") == 0) httpd_resp_send_chunk(req, " selected", 9);
    httpd_resp_send_chunk(req, ">Deutsch</option></select></label>", HTTPD_RESP_USE_STRLEN);

    // Wi-Fi
    httpd_resp_send_chunk(req, "<h2>Wi-Fi</h2>"
        "<label>Network (SSID)<input name=\"ssid\" type=\"text\" autocomplete=\"off\""
        " autocapitalize=\"none\" spellcheck=\"false\" required maxlength=\"32\" value=\"",
        HTTPD_RESP_USE_STRLEN);
    html_escape_chunk(req, ssid);
    httpd_resp_send_chunk(req, "\"></label>"
        "<label>Password<input name=\"pass\" type=\"password\" autocomplete=\"new-password\""
        " maxlength=\"63\" value=\"",
        HTTPD_RESP_USE_STRLEN);
    html_escape_chunk(req, pass);
    httpd_resp_send_chunk(req, "\"></label>", HTTPD_RESP_USE_STRLEN);

    // Station
    httpd_resp_send_chunk(req, "<h2>Station</h2>"
        "<label>Station (type to search)<input name=\"station\" list=\"be_stations\""
        " type=\"text\" autocomplete=\"off\" autocapitalize=\"words\" spellcheck=\"false\""
        " required maxlength=\"48\" value=\"",
        HTTPD_RESP_USE_STRLEN);
    html_escape_chunk(req, station);
    httpd_resp_send_chunk(req, "\"></label>", HTTPD_RESP_USE_STRLEN);

    // Night dim
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<h2>Night dim</h2>"
        "<p class=\"note\">Display off between these hours (0..23, set both to 0 to disable).</p>"
        "<div class=\"row\">"
        "<label>From<input name=\"dim_s\" type=\"number\" min=\"0\" max=\"23\" value=\"%u\"></label>"
        "<label>To<input name=\"dim_e\" type=\"number\" min=\"0\" max=\"23\" value=\"%u\"></label>"
        "</div>", (unsigned)dim_s, (unsigned)dim_e);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    // Weather
    snprintf(buf, sizeof(buf),
        "<h2>Weather (optional)</h2>"
        "<p class=\"note\">Coordinates for the header thermometer. Leave at 0 to hide.</p>"
        "<div class=\"row\">"
        "<label>Latitude<input name=\"wx_lat\" type=\"number\" step=\"0.0001\""
        " value=\"%.4f\"></label>"
        "<label>Longitude<input name=\"wx_lon\" type=\"number\" step=\"0.0001\""
        " value=\"%.4f\"></label></div>",
        have_wx ? wx_lat : 0.0f, have_wx ? wx_lon : 0.0f);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<button type=\"submit\">Save and restart</button>"
        "</form>",
        HTTPD_RESP_USE_STRLEN);

    // OTA section
    httpd_resp_send_chunk(req,
        "<h2>Firmware</h2>"
        "<p class=\"note\">Current version: " APP_VERSION "</p>"
        "<form method=\"POST\" action=\"/ota\">"
        "<button class=\"secondary\" type=\"submit\">Check for updates & install</button>"
        "</form>",
        HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<p class=\"foot\">moshon-timetable v" APP_VERSION
        " | <a style=\"color:#67c\" href=\"" APP_HOMEPAGE_SHORT "\">"
        APP_HOMEPAGE_SHORT "</a></p><datalist id=\"be_stations\">",
        HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, STATIONS_BE_HTML_OPTIONS, STATIONS_BE_HTML_OPTIONS_LEN);
    httpd_resp_send_chunk(req, HTML_AFTER_STATIONS, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ----------------------------------------------------------------------
// POST /save — same semantics as the captive-portal /save
// ----------------------------------------------------------------------
static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "restarting to pick up new settings");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) break;
        total += r;
        remaining -= r;
    }
    body[total] = '\0';

    char ssid[CFG_FIELD_MAX]    = {0};
    char pass[CFG_FIELD_MAX]    = {0};
    char station[CFG_FIELD_MAX] = {0};
    char lang[8]                = {0};
    char dim_s_str[8] = {0}, dim_e_str[8] = {0};
    char wx_lat_str[24] = {0}, wx_lon_str[24] = {0};

    httpd_query_key_value(body, "ssid",    ssid,    sizeof(ssid));
    httpd_query_key_value(body, "pass",    pass,    sizeof(pass));
    httpd_query_key_value(body, "station", station, sizeof(station));
    httpd_query_key_value(body, "lang",    lang,    sizeof(lang));
    httpd_query_key_value(body, "dim_s",   dim_s_str, sizeof(dim_s_str));
    httpd_query_key_value(body, "dim_e",   dim_e_str, sizeof(dim_e_str));
    httpd_query_key_value(body, "wx_lat",  wx_lat_str, sizeof(wx_lat_str));
    httpd_query_key_value(body, "wx_lon",  wx_lon_str, sizeof(wx_lon_str));

    url_decode_inplace(ssid);
    url_decode_inplace(pass);
    url_decode_inplace(station);
    url_decode_inplace(lang);
    url_decode_inplace(dim_s_str);
    url_decode_inplace(dim_e_str);
    url_decode_inplace(wx_lat_str);
    url_decode_inplace(wx_lon_str);

    if (lang[0])    cfg_save_language(lang);
    if (ssid[0])    cfg_save_wifi(ssid, pass);
    if (station[0]) cfg_save_station(station);

    long dim_s = strtol(dim_s_str, NULL, 10);
    long dim_e = strtol(dim_e_str, NULL, 10);
    if (dim_s >= 0 && dim_s <= 23 && dim_e >= 0 && dim_e <= 23) {
        cfg_save_dim_hours((uint8_t)dim_s, (uint8_t)dim_e);
    }

    if (wx_lat_str[0] && wx_lon_str[0]) {
        float lat = strtof(wx_lat_str, NULL);
        float lon = strtof(wx_lon_str, NULL);
        cfg_save_weather(lat, lon);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"6;url=/\">"
        "<style>body{font-family:sans-serif;background:#0A1F44;color:#fff;"
        "padding:40px;text-align:center}h1{color:#FCD500}</style></head><body>"
        "<h1>Saved</h1><p>The device is restarting. This page will reload in 6s.</p>"
        "</body></html>");

    xTaskCreate(restart_task, "save_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ----------------------------------------------------------------------
// POST /ota — pull the firmware asset from the latest GitHub release
// ----------------------------------------------------------------------
#ifndef CONFIG_OTA_FIRMWARE_URL
#define CONFIG_OTA_FIRMWARE_URL \
    "https://github.com/matsvandamme/moshon-timetable/releases/latest/download/moshon_timetable.bin"
#endif

static void ota_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "OTA: fetching %s", CONFIG_OTA_FIRMWARE_URL);

    esp_http_client_config_t http_cfg = {
        .url = CONFIG_OTA_FIRMWARE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA: image flashed successfully, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<style>body{font-family:sans-serif;background:#0A1F44;color:#fff;"
        "padding:40px;text-align:center}h1{color:#FCD500}</style></head><body>"
        "<h1>Updating...</h1>"
        "<p>The device is downloading the latest firmware. This will take 1-2 minutes.</p>"
        "<p>It will reboot automatically once the new image is ready. Reload this page after a few minutes.</p>"
        "</body></html>");

    // Run the actual OTA work in a separate task so the response goes out.
    // 12 KB stack: mbedtls TLS handshake against github.com is heavier than
    // iRail's. The OTA task itself terminates on success (via esp_restart)
    // or on failure (vTaskDelete).
    xTaskCreate(ota_task, "ota", 12 * 1024, NULL, 5, NULL);
    return ESP_OK;
}

// ----------------------------------------------------------------------
// mDNS
// ----------------------------------------------------------------------
static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set("moshon-timetable");
    mdns_instance_name_set("moshon-timetable settings");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS up: moshon-timetable.local");
    return ESP_OK;
}

// ----------------------------------------------------------------------
// Public lifecycle
// ----------------------------------------------------------------------
esp_err_t settings_web_start(void)
{
    if (s_httpd) return ESP_OK;

    start_mdns();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t r_form = { .uri = "/",     .method = HTTP_GET,  .handler = serve_form };
    httpd_uri_t r_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t r_ota  = { .uri = "/ota",  .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(s_httpd, &r_form);
    httpd_register_uri_handler(s_httpd, &r_save);
    httpd_register_uri_handler(s_httpd, &r_ota);

    ESP_LOGI(TAG, "Settings web up on :80 (visit http://moshon-timetable.local/)");
    return ESP_OK;
}

esp_err_t settings_web_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    mdns_free();
    return ESP_OK;
}
