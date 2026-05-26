# moshon-timetable

[![build](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml/badge.svg)](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml)
[![release](https://github.com/matsvandamme/moshon-timetable/actions/workflows/release.yml/badge.svg)](https://github.com/matsvandamme/moshon-timetable/releases/latest)

NMBS / SNCB train departures and arrivals on a desk-sized passenger-info
display. Pulls live data from the [iRail public API](https://docs.irail.be/),
renders an NMBS-styled board on a Wireless-Tag **WT32-SC01 Plus**
(ESP32-S3 + 3.5" 480×320 IPS + capacitive touch), using ESP-IDF v5.3 and
LVGL v9.

## Features

- Authentic NMBS look — navy background, yellow accents, embedded
  van de Velde **B**-in-oval logo rasterised from the official SVG
- **Tap anywhere on the screen** to swap between Vertrek (departures)
  and Aankomst (arrivals)
- **Synchronised via-stop cycling** — every multi-stop row advances
  through its pages on the same global tick; single-stop rows stay put
- **No past times ever** — entries whose scheduled time + delay are in
  the past are filtered out; pagination looks forward in time until 5
  future trains are found (handles overnight lulls automatically)
- **Wi-Fi splash overlay** with NMBS logo, build version, ESP-IDF version
  and developer name — held on screen for at least 4 s so you can read it
- **Demo mode** (Kconfig flag) populates synthetic boards with delays
  and cancellations for screenshots / offline iteration
- **Per-train via-stop lookup** via iRail `/vehicle/?id=…` with a 5-min
  TTL cache so we stay well inside the 3 req/s API budget
- Ticking clock with a blinking colon, white-outlined platform badges,
  red delay / cancellation indicators

## Hardware

| Item | Value |
|---|---|
| Board | Wireless-Tag **WT32-SC01 Plus** (Seeed SKU BH2306250001) |
| MCU | ESP32-S3 (WT32-S3-WROVER module) — 16 MB flash, 2 MB **quad** PSRAM |
| Display | 3.5" IPS 480×320, **ST7796** via 8-bit i80 parallel |
| Touch | **FT6336U** over I²C |
| USB | Native USB-CDC on the S3 (VID `0x303A` / PID `0x1001`) |

Full pin map is in [`main/display/bsp.h`](main/display/bsp.h). Notable
quirks that took some hardware-level iteration:

- PSRAM is **quad** mode (not octal — despite the WT32-S3-WROVER name).
- ST7796 driver wants `LCD_RGB_ELEMENT_ORDER_BGR` *and* a manual
  16-bit byte swap in the flush callback to render NMBS colours correctly.
- `invert_color` polarity is inverted on this panel — pass `true` to get
  normal display.

## Quick start — flash a prebuilt release

The easiest path. No toolchain install required, just `esptool`.

1. Plug the board into USB. It enumerates as a USB-CDC serial port
   (Windows: `COMx`, macOS: `/dev/cu.usbmodem*`, Linux: `/dev/ttyACM0`).
2. Grab the **`moshon_timetable-full.bin`** asset from the
   [latest release](https://github.com/matsvandamme/moshon-timetable/releases/latest).
   This is a single image already merged with the bootloader and
   partition table.
3. Install esptool if you don't have it:
   ```sh
   pip install esptool
   ```
4. Flash in one shot:
   ```sh
   esptool.py --chip esp32s3 --port COM11 write_flash 0x0 moshon_timetable-full.bin
   ```
   (Substitute your port name. The chip is auto-reset after flashing.)

Once it boots you'll see the splash screen. **Out of the box the prebuilt
firmware has no Wi-Fi credentials.** Until the AP-mode provisioning lands
(planned), you need to build from source with your own creds — see below.

## Build from source

### Prerequisites

- [ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/index.html)
  installed for the `esp32s3` target. On Windows the official Espressif
  installer is the easiest path. On Linux/macOS:
  ```sh
  mkdir -p ~/esp && cd ~/esp
  git clone --depth=1 --branch v5.3.2 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32s3
  . ./export.sh
  ```
- Real Python ≥ 3.9 on `PATH` (Windows users: the Microsoft Store stub
  doesn't count — install from python.org or via `winget install Python.Python.3.12`).

### Configure your Wi-Fi credentials

```sh
cp main/wifi_secrets.h.example main/wifi_secrets.h
# then edit main/wifi_secrets.h and set WIFI_SSID / WIFI_PASSWORD
```

`main/wifi_secrets.h` is gitignored — your credentials never end up in a
commit or a CI artifact.

### Build, flash, monitor

```sh
. $IDF_PATH/export.sh                # activates idf.py on PATH
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor # substitute your port
```

`Ctrl+]` exits the serial monitor.

### Configure (optional)

```sh
idf.py menuconfig
```

Go to **Moshon Timetable** to change:

- `Active NMBS station` — pick from 12 presets or enter a custom name
- `Refresh period (seconds)` — how often to poll iRail (default 45)
- `Demo mode` — synthesise boards locally, skip Wi-Fi entirely
- `Departures/Arrivals toggle period` — for the auto-toggle (currently
  unused; the UI is touch-driven)

## Project layout

```
firmware/
├── .github/workflows/      # build (per push) + release (per tag)
├── main/
│   ├── main.c              # orchestrator: NVS → BSP → UI → Wi-Fi → refresh loop
│   ├── app_config.h        # version, user-agent, etc.
│   ├── Kconfig.projbuild   # menuconfig options
│   ├── stations.{c,h}      # 12-station preset table + active selector
│   ├── demo.{c,h}          # synthetic board for CONFIG_DEMO_MODE=y
│   ├── wifi_secrets.h.example
│   ├── display/
│   │   ├── bsp.h           # WT32-SC01 Plus pin map + public BSP API
│   │   └── bsp.c           # esp_lcd ST7796 + FT6336 + LVGL port
│   ├── net/
│   │   ├── wifi.{c,h}      # STA + SNTP
│   │   ├── irail.{c,h}     # liveboard fetcher + cJSON parser + pagination
│   │   └── irail_vehicle.{c,h}  # /vehicle/ fetcher + via-stop cache
│   ├── ui/
│   │   ├── nmbs_theme.h    # colour palette
│   │   └── ui.{c,h}        # header + 5 rows + Wi-Fi splash
│   └── assets/
│       ├── nmbs_logo.{c,h} # rasterised SVG, 56×36 ARGB8888
│       └── sncb_logo_src.png
├── scripts/
│   └── conv_logo.py        # regenerates nmbs_logo.c from sncb_logo_src.png
├── partitions.csv
├── sdkconfig.defaults      # PSRAM, mbedTLS bundle, LVGL fonts, hw crypto, ...
└── CMakeLists.txt
```

## iRail usage

This firmware identifies itself with a descriptive `User-Agent` per the
[iRail TOS](https://docs.irail.be/) and stays well inside the documented
**3 requests per second / 5 burst** budget:

- One liveboard per direction every `REFRESH_PERIOD_SECONDS` (default 45 s)
- Up to a handful of `/vehicle/` calls per refresh to fill the via-stop
  cache, then cache hits for the next 5 minutes

iRail is a free volunteer-run service. Please don't crank the refresh
period below ~30 s without a reason.

## Continuous integration

- **`build.yml`** — every push and pull request builds the firmware for
  `esp32s3` via the official `espressif/esp-idf-ci-action`, uploads the
  bin/elf/bootloader/partition-table as artifacts (retained 30 days).
- **`release.yml`** — pushing a `v*` tag (e.g. `v0.1.0`) builds, merges
  bootloader+partition+app into `moshon_timetable-full.bin` with
  `esptool.py merge_bin`, and publishes a GitHub Release with auto-generated
  release notes.

CI uses a placeholder `wifi_secrets.h` (copied from the `.example`) so
the artifacts contain no real credentials.

## Trademark notice

The NMBS / SNCB **B**-in-oval mark (designed by Henry van de Velde in 1936)
is a registered trademark of NMBS / SNCB. It is embedded in this firmware
solely to identify the operator whose live data the device displays — the
canonical fair-use scenario. The bundled image is rasterised from the
publicly-available SVG on Wikimedia Commons. If you redistribute the
firmware in modified form, please continue to use it only for
identification of NMBS / SNCB train information.

## Author

[Matthieu Van Damme](https://github.com/matsvandamme) · VMAT

## License

MIT — see [LICENSE](LICENSE). Third-party components retain their
respective licences (ESP-IDF: Apache 2.0; LVGL: MIT).
