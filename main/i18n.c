// String table + NVS-backed language selection.

#include "i18n.h"
#include "cfg.h"
#include "esp_log.h"

#include <string.h>
#include <strings.h>   // strcasecmp

static const char *TAG = "i18n";
static lang_t s_lang = LANG_NL;

// Translations table — rows are TR_*, columns are LANG_*.
// Keep entries short enough to fit the column widths used in ui.c.
// Use UTF-8 for accented characters (LVGL Montserrat 14/20/24 only covers
// basic Latin so we limit accents to ASCII fallbacks where possible).
static const char *const TR[TR_COUNT][LANG_COUNT] = {
    // NL,           FR,                EN,             DE
    [TR_DEPARTURES]      = { "Vertrek",      "Departs",         "Departures",    "Abfahrt" },
    [TR_ARRIVALS]        = { "Aankomst",     "Arrivees",        "Arrivals",      "Ankunft" },
    [TR_CONNECTING]      = { "Verbinden...", "Connexion...",    "Connecting...", "Verbinde..." },
    [TR_RECONNECTING]    = { "Herverbinden...","Reconnexion...","Reconnecting...","Wiederverbinden..." },
    [TR_FETCHING_DATA]   = { "Data ophalen...","Chargement...", "Loading...",    "Lade Daten..." },
    [TR_SETUP_MODE]      = { "Setup",         "Configuration",  "Setup mode",    "Einrichtung" },
    [TR_CONNECT_TO_WIFI] = { "Verbind met Wi-Fi:","Connectez-vous au Wi-Fi :","Connect to Wi-Fi:","Mit WLAN verbinden:" },
    [TR_RESET]           = { "Reset...",      "Reset...",       "Reset...",      "Reset..." },
    [TR_CANCELLED_ABBR]  = { "AFG",           "SUPP",           "CANC",          "ENT" },
    [TR_VIA]             = { "via ",          "via ",           "via ",          "via " },
};

void i18n_init(void)
{
    char iso[8] = {0};
    if (cfg_load_language(iso, sizeof(iso)) == ESP_OK) {
        lang_t parsed;
        if (i18n_parse_iso(iso, &parsed)) {
            s_lang = parsed;
        }
    }
    ESP_LOGI(TAG, "active language: %s", i18n_iso());
}

lang_t i18n_get(void) { return s_lang; }

void i18n_set(lang_t lang)
{
    if (lang < 0 || lang >= LANG_COUNT) return;
    s_lang = lang;
    cfg_save_language(i18n_iso());
}

const char *i18n_iso(void)
{
    switch (s_lang) {
        case LANG_NL: return "nl";
        case LANG_FR: return "fr";
        case LANG_EN: return "en";
        case LANG_DE: return "de";
        default:      return "nl";
    }
}

const char *i18n_text(tr_t id)
{
    return i18n_text_in(s_lang, id);
}

const char *i18n_text_in(lang_t lang, tr_t id)
{
    if (id < 0 || id >= TR_COUNT)         return "";
    if (lang < 0 || lang >= LANG_COUNT)   lang = LANG_NL;
    const char *s = TR[id][lang];
    return s ? s : "";
}

bool i18n_parse_iso(const char *iso, lang_t *out)
{
    if (!iso || !out) return false;
    if (strcasecmp(iso, "nl") == 0) { *out = LANG_NL; return true; }
    if (strcasecmp(iso, "fr") == 0) { *out = LANG_FR; return true; }
    if (strcasecmp(iso, "en") == 0) { *out = LANG_EN; return true; }
    if (strcasecmp(iso, "de") == 0) { *out = LANG_DE; return true; }
    return false;
}
