#pragma once

// Runtime UI language. Picked once in the captive-portal setup form and
// persisted in NVS; loaded back at boot. Affects on-screen labels and the
// `lang` parameter sent to iRail.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_NL = 0,   // Dutch    (default — matches the operator's primary tongue)
    LANG_FR,       // French
    LANG_EN,       // English
    LANG_DE,       // German
    LANG_COUNT,
} lang_t;

// Translation slots. Add new ones at the END to preserve numbering.
typedef enum {
    TR_DEPARTURES = 0,    // header tab
    TR_ARRIVALS,
    TR_CONNECTING,        // boot splash overlay
    TR_RECONNECTING,      // wifi loss overlay
    TR_FETCHING_DATA,     // wifi up, no entries yet
    TR_SETUP_MODE,        // AP-mode headline
    TR_CONNECT_TO_WIFI,   // AP-mode subtitle prefix
    TR_RESET,             // long-press confirmation
    TR_CANCELLED_ABBR,    // delay badge "AFG"/"SUPP"/"CANC"/"ENT"
    TR_VIA,               // "via " prefix
    TR_COUNT,
} tr_t;

// Load the saved language from NVS (defaults to LANG_NL on cache miss).
// Idempotent — safe to call once at boot.
void  i18n_init(void);

// Get / set the active language. i18n_set persists to NVS.
lang_t i18n_get(void);
void   i18n_set(lang_t lang);

// ISO-639-1 two-letter code for the active language ("nl" / "fr" / "en"
// / "de"). Used as the `lang=` query param when calling iRail.
const char *i18n_iso(void);

// Lookup a translated string for the current language.
const char *i18n_text(tr_t id);

// Same but for an explicitly-requested language — useful when rendering
// the language picker that shows each option in its own tongue.
const char *i18n_text_in(lang_t lang, tr_t id);

// Parse "nl"/"fr"/"en"/"de" (case-insensitive). Returns true on success.
bool i18n_parse_iso(const char *iso, lang_t *out);

#ifdef __cplusplus
}
#endif
