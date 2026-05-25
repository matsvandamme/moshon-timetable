# moshon-timetable

NMBS / SNCB train departures and arrivals for **Aalter** station, on a Seeed Studio **WT32-SC01 Plus** (ESP32-S3, 3.5" 480×320 IPS, FT6336 touch).

Live data from the [iRail public API](https://docs.irail.be/), UI in [LVGL](https://lvgl.io/) v9.

![status](https://img.shields.io/badge/status-early-orange)

## Hardware

| Item | Value |
|---|---|
| Board | Wireless-Tag WT32-SC01 Plus (Seeed BH2306250001) |
| MCU | ESP32-S3 (WT32-S3-WROVER module, 16 MB flash, 8 MB octal PSRAM) |
| Display | 3.5" IPS 480×320, ST7796 via 8-bit i80 parallel |
| Touch | FT6336U over I²C |
| USB | Native USB-CDC on the S3 (VID 0x303A / PID 0x1001) |

Pin map: see [`main/display/bsp.h`](main/display/bsp.h).

## Build & flash

```powershell
# 1. Activate ESP-IDF (PowerShell)
& "$env:USERPROFILE\.espressif\activate_idf_*.ps1"

# 2. Copy your Wi-Fi creds (never committed)
copy main\wifi_secrets.h.example main\wifi_secrets.h
# then edit main\wifi_secrets.h with your SSID and password

# 3. Configure target + flash + monitor
idf.py set-target esp32s3
idf.py -p COM11 flash monitor
```

`Ctrl+]` exits the serial monitor.

## Project layout

```
firmware/
├── main/
│   ├── main.c              # orchestrator: NVS → BSP → UI → Wi-Fi → refresh loop
│   ├── app_config.h        # station id, refresh period, user-agent
│   ├── wifi_secrets.h.example
│   ├── display/
│   │   ├── bsp.h           # WT32-SC01 Plus pin map + public BSP API
│   │   └── bsp.c           # esp_lcd ST7796 + FT6336 + LVGL port
│   ├── net/
│   │   ├── wifi.h/.c       # STA + SNTP
│   │   └── irail.h/.c      # HTTPS liveboard fetcher + cJSON parser
│   └── ui/
│       ├── nmbs_theme.h    # NMBS colour palette
│       ├── ui.h/.c         # header + departures/arrivals + footer
├── partitions.csv
├── sdkconfig.defaults      # PSRAM, mbedTLS bundle, LVGL fonts
└── CMakeLists.txt
```

## iRail usage

This client identifies itself with a descriptive User-Agent per the [iRail TOS](https://docs.irail.be/) and polls the `liveboard` endpoint at **`REFRESH_PERIOD_MS`** intervals (default 45 s). Don't crank that lower without a reason — iRail is a free volunteer service.

## License

MIT.
