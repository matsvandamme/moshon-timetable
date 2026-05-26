#include "stations.h"
#include "sdkconfig.h"
#include "cfg.h"
#include <string.h>

// Curated list of common Belgian stations. Held as a single array so the
// compiler doesn't whine about the entries we don't select via Kconfig
// (`-Werror=unused-const-variable`). To add a new entry: extend `stations[]`,
// add a matching `IDX_*` enum value, add a `config STATION_*` line in
// Kconfig.projbuild, and add an `elif defined(...)` branch below.
//
// `query_name` is what iRail accepts in `?station=...`. For the canonical
// names below the API resolves both Dutch and French variants automatically.

enum {
    IDX_AALTER,
    IDX_BRUSSELS_SOUTH,
    IDX_BRUSSELS_CENTRAL,
    IDX_BRUSSELS_NORTH,
    IDX_ANTWERPEN_CENTRAAL,
    IDX_GENT_SINT_PIETERS,
    IDX_BRUGGE,
    IDX_OOSTENDE,
    IDX_LEUVEN,
    IDX_LIEGE_GUILLEMINS,
    IDX_NAMUR,
    IDX_CHARLEROI_SUD,
    IDX_CUSTOM,
};

static const station_t stations[] = {
    [IDX_AALTER]             = { "Aalter",             "Aalter"             },
    [IDX_BRUSSELS_SOUTH]     = { "Brussels-South",     "Brussels-South"     },
    [IDX_BRUSSELS_CENTRAL]   = { "Brussels-Central",   "Brussels-Central"   },
    [IDX_BRUSSELS_NORTH]     = { "Brussels-North",     "Brussels-North"     },
    [IDX_ANTWERPEN_CENTRAAL] = { "Antwerpen-Centraal", "Antwerpen-Centraal" },
    [IDX_GENT_SINT_PIETERS]  = { "Gent-Sint-Pieters",  "Gent-Sint-Pieters"  },
    [IDX_BRUGGE]             = { "Brugge",             "Brugge"             },
    [IDX_OOSTENDE]           = { "Oostende",           "Oostende"           },
    [IDX_LEUVEN]             = { "Leuven",             "Leuven"             },
    [IDX_LIEGE_GUILLEMINS]   = { "Liege-Guillemins",   "Liege-Guillemins"   },
    [IDX_NAMUR]              = { "Namur",              "Namur"              },
    [IDX_CHARLEROI_SUD]      = { "Charleroi-Sud",      "Charleroi-Sud"      },
#ifdef CONFIG_STATION_CUSTOM
    [IDX_CUSTOM]             = { CONFIG_STATION_CUSTOM_NAME, CONFIG_STATION_CUSTOM_NAME },
#endif
};

// If the user provisioned a station via the AP setup form, NVS holds the
// chosen Dutch name. Prefer that over the Kconfig default — otherwise the
// device would keep displaying "Aalter" even after a user typed something
// else in the captive portal. Display name == query name (iRail accepts
// both Dutch and French/English names).
static char        s_nvs_name[64];
static station_t   s_nvs_station;
static bool        s_nvs_loaded;

static const station_t *try_nvs_station(void)
{
    if (!s_nvs_loaded) {
        s_nvs_loaded = true;
        if (cfg_load_station(s_nvs_name, sizeof(s_nvs_name)) == ESP_OK
            && s_nvs_name[0]) {
            s_nvs_station.display_name = s_nvs_name;
            s_nvs_station.query_name   = s_nvs_name;
        }
    }
    return s_nvs_name[0] ? &s_nvs_station : NULL;
}

const station_t *station_get_active(void)
{
    const station_t *nvs = try_nvs_station();
    if (nvs) return nvs;

#if   defined(CONFIG_STATION_AALTER)
    return &stations[IDX_AALTER];
#elif defined(CONFIG_STATION_BRUSSELS_SOUTH)
    return &stations[IDX_BRUSSELS_SOUTH];
#elif defined(CONFIG_STATION_BRUSSELS_CENTRAL)
    return &stations[IDX_BRUSSELS_CENTRAL];
#elif defined(CONFIG_STATION_BRUSSELS_NORTH)
    return &stations[IDX_BRUSSELS_NORTH];
#elif defined(CONFIG_STATION_ANTWERPEN_CENTRAAL)
    return &stations[IDX_ANTWERPEN_CENTRAAL];
#elif defined(CONFIG_STATION_GENT_SINT_PIETERS)
    return &stations[IDX_GENT_SINT_PIETERS];
#elif defined(CONFIG_STATION_BRUGGE)
    return &stations[IDX_BRUGGE];
#elif defined(CONFIG_STATION_OOSTENDE)
    return &stations[IDX_OOSTENDE];
#elif defined(CONFIG_STATION_LEUVEN)
    return &stations[IDX_LEUVEN];
#elif defined(CONFIG_STATION_LIEGE_GUILLEMINS)
    return &stations[IDX_LIEGE_GUILLEMINS];
#elif defined(CONFIG_STATION_NAMUR)
    return &stations[IDX_NAMUR];
#elif defined(CONFIG_STATION_CHARLEROI_SUD)
    return &stations[IDX_CHARLEROI_SUD];
#elif defined(CONFIG_STATION_CUSTOM)
    return &stations[IDX_CUSTOM];
#else
    return &stations[IDX_AALTER];
#endif
}
