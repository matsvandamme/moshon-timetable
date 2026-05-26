// SoftAP + HTTP setup form + DNS captive portal.
//
// Triggered when NVS has no Wi-Fi credentials (first boot, or after a
// long-press wipe). User joins the open "moshon-setup" network, the
// phone's captive-portal probe is hijacked by our DNS responder, the
// setup form opens automatically. User picks SSID, types password,
// picks a station from the preset dropdown (or enters a custom name),
// submits. We save to NVS and reboot — next boot uses the saved creds.

#include "provision_ap.h"
#include "cfg.h"
#include "app_config.h"
#include "stations_be.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "prov";

#define AP_IP_ADDR0   192
#define AP_IP_ADDR1   168
#define AP_IP_ADDR2   4
#define AP_IP_ADDR3   1
#define AP_IP_PACKED  ((AP_IP_ADDR0 << 24) | (AP_IP_ADDR1 << 16) | \
                       (AP_IP_ADDR2 << 8)  | AP_IP_ADDR3)
#define DNS_PORT      53
#define DNS_MAX       512

static httpd_handle_t s_httpd = NULL;

// -------------------------------------------------------------------
// HTML page — split so we can stream it chunk-by-chunk and inject the
// full Belgian-stations datalist (~21 KB, see stations_be.c) in the
// middle without one giant static string.
// -------------------------------------------------------------------
static const char HTML_HEAD[] =
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>moshon-timetable setup</title>"
    "<style>"
    "html,body{margin:0;padding:0}"
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
    "background:#0A1F44;color:#fff;padding:20px;max-width:480px;margin:0 auto;"
    "min-height:100vh;box-sizing:border-box}"
    "h1{margin:0 0 6px;color:#FCD500;font-size:22px}"
    "p.sub{margin:0 0 22px;color:#aab;font-size:14px}"
    "form{display:flex;flex-direction:column;gap:14px}"
    "label{display:flex;flex-direction:column;gap:4px;font-size:13px;"
    "color:#aab;text-transform:uppercase;letter-spacing:.5px}"
    "input,select{padding:11px 12px;font-size:16px;border:1px solid #fff3;"
    "border-radius:8px;background:#002F6C;color:#fff;box-sizing:border-box}"
    "input:focus,select:focus{outline:2px solid #FCD500;border-color:transparent}"
    "button{margin-top:8px;padding:14px;background:#FCD500;color:#002F6C;"
    "border:0;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}"
    "button:active{opacity:.85}"
    ".foot{margin-top:30px;font-size:11px;color:#67c;text-align:center}"
    "</style></head><body>"
    "<h1>moshon-timetable</h1>"
    "<p class=\"sub\">Connect the display to your Wi-Fi and pick a station.</p>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>Wi-Fi network (SSID)"
    "<input name=\"ssid\" type=\"text\" autocomplete=\"off\" autocapitalize=\"none\""
    " spellcheck=\"false\" required maxlength=\"32\"></label>"
    "<label>Wi-Fi password"
    "<input name=\"pass\" type=\"password\" autocomplete=\"new-password\""
    " maxlength=\"63\"></label>"
    "<label>Station (type to search)"
    "<input name=\"station\" list=\"be_stations\" type=\"text\""
    " autocomplete=\"off\" autocapitalize=\"words\" spellcheck=\"false\""
    " required maxlength=\"48\"></label>"
    "<button type=\"submit\">Save and restart</button>"
    "</form>"
    "<datalist id=\"be_stations\">";

static const char HTML_TAIL[] =
    "</datalist>"
    "<p class=\"foot\">moshon-timetable v" APP_VERSION " \xE2\x80\xA2 by " APP_DEVELOPER "</p>"
    "</body></html>";

static const char HTML_SAVED[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Saved</title><style>body{font-family:-apple-system,sans-serif;"
    "background:#0A1F44;color:#fff;padding:40px;text-align:center}"
    "h1{color:#FCD500}</style></head><body>"
    "<h1>Saved \xE2\x9C\x93</h1>"
    "<p>The device will restart and connect to your Wi-Fi.</p>"
    "<p>You can disconnect from <b>moshon-setup</b> now.</p>"
    "</body></html>";

// -------------------------------------------------------------------
// URL-decode application/x-www-form-urlencoded value in place.
// -------------------------------------------------------------------
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

// -------------------------------------------------------------------
// HTTP handlers
// -------------------------------------------------------------------

static esp_err_t serve_setup_form(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);
    // All ~714 Belgian stations as pre-rendered <option> tags. Streamed
    // in one big chunk; total payload is ~20 KB.
    httpd_resp_send_chunk(req, STATIONS_BE_HTML_OPTIONS,
                          STATIONS_BE_HTML_OPTIONS_LEN);
    httpd_resp_send_chunk(req, HTML_TAIL, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "restarting to pick up new credentials");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[768];
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total,
                               sizeof(body) - 1 - total);
        if (r <= 0) break;
        total += r;
        remaining -= r;
    }
    body[total] = '\0';

    char ssid[CFG_FIELD_MAX]    = {0};
    char pass[CFG_FIELD_MAX]    = {0};
    char station[CFG_FIELD_MAX] = {0};

    httpd_query_key_value(body, "ssid",    ssid,    sizeof(ssid));
    httpd_query_key_value(body, "pass",    pass,    sizeof(pass));
    httpd_query_key_value(body, "station", station, sizeof(station));
    url_decode_inplace(ssid);
    url_decode_inplace(pass);
    url_decode_inplace(station);

    if (ssid[0]) {
        cfg_save_wifi(ssid, pass);
    }
    // datalist input accepts both autocompleted picks AND arbitrary text,
    // so whatever ended up in `station` is what the user wants.
    if (station[0]) {
        cfg_save_station(station);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_SAVED, HTTPD_RESP_USE_STRLEN);

    // Restart in a separate task so the response actually goes out.
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Catch-all GET — same form. Used so that Android/iOS captive-portal
// probes get a proper page rather than a 404.
static esp_err_t catchall_get(httpd_req_t *req)
{
    return serve_setup_form(req);
}

// -------------------------------------------------------------------
// Minimal DNS responder: answer every A query with the AP's IP, so the
// phone's captive-portal probe (and any browser hostname lookup) lands
// back on us and the setup page opens automatically.
// -------------------------------------------------------------------
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket() failed");
        vTaskDelete(NULL);
    }
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DNS_PORT),
    };
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind() failed");
        close(sock); vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "DNS captive listener up on UDP/53");

    uint8_t buf[DNS_MAX];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        // Build the response in-place over the request: flip QR to 1,
        // set ANCOUNT=1, append one A record pointing at our AP IP.
        if (n + 16 > (int)sizeof(buf)) continue;
        buf[2] |= 0x80;             // QR = 1
        buf[3] |= 0x80;             // RA = 1
        buf[6] = 0; buf[7] = 1;     // ANCOUNT = 1
        buf[8] = 0; buf[9] = 0;     // NSCOUNT = 0
        buf[10] = 0; buf[11] = 0;   // ARCOUNT = 0

        int pos = n;
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;             // name pointer
        buf[pos++] = 0x00; buf[pos++] = 0x01;             // type A
        buf[pos++] = 0x00; buf[pos++] = 0x01;             // class IN
        buf[pos++] = 0x00; buf[pos++] = 0x00;             // TTL hi
        buf[pos++] = 0x00; buf[pos++] = 0x3C;             // TTL lo (60s)
        buf[pos++] = 0x00; buf[pos++] = 0x04;             // RDLENGTH
        buf[pos++] = AP_IP_ADDR0;
        buf[pos++] = AP_IP_ADDR1;
        buf[pos++] = AP_IP_ADDR2;
        buf[pos++] = AP_IP_ADDR3;

        sendto(sock, buf, pos, 0,
               (struct sockaddr *)&client, clen);
    }
}

// -------------------------------------------------------------------
// Public entry point
// -------------------------------------------------------------------
esp_err_t provision_ap_start(void)
{
    ESP_LOGW(TAG, "Entering AP-mode provisioning ('" PROVISION_AP_SSID "')");

    // Bring up Wi-Fi stack in SoftAP mode.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = strlen(PROVISION_AP_SSID),
            .channel        = 1,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, PROVISION_AP_SSID,
            sizeof(ap_cfg.ap.ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Pin AP to 192.168.4.1/24 so the DNS portal and the URL we show
    // on the splash match.
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip,      AP_IP_ADDR0, AP_IP_ADDR1, AP_IP_ADDR2, AP_IP_ADDR3);
    IP4_ADDR(&ip.gw,      AP_IP_ADDR0, AP_IP_ADDR1, AP_IP_ADDR2, AP_IP_ADDR3);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    // HTTP server with a catch-all so any URL the phone probes returns
    // the setup form (helps trigger native captive-portal detection).
    httpd_config_t hcfg     = HTTPD_DEFAULT_CONFIG();
    hcfg.lru_purge_enable   = true;
    hcfg.uri_match_fn       = httpd_uri_match_wildcard;
    hcfg.max_uri_handlers   = 4;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hcfg));

    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST,
                         .handler = save_post };
    httpd_register_uri_handler(s_httpd, &save);

    httpd_uri_t any = { .uri = "/*", .method = HTTP_GET,
                        .handler = catchall_get };
    httpd_register_uri_handler(s_httpd, &any);

    // DNS captive portal — hijack every name lookup back to us.
    xTaskCreate(dns_task, "dns_captive", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "AP ready: SSID='" PROVISION_AP_SSID
                  "', URL=" PROVISION_AP_URL);
    return ESP_OK;
}
