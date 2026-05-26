<div align="center">

# moshon-timetable

**Live NMBS / SNCB train departures and arrivals on a 3.5″ desktop display.**

[![build](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml/badge.svg)](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/matsvandamme/moshon-timetable?style=flat-square&color=ffd200&labelColor=003c7e&label=release)](https://github.com/matsvandamme/moshon-timetable/releases/latest)
[![license](https://img.shields.io/github/license/matsvandamme/moshon-timetable?style=flat-square&color=ffd200&labelColor=003c7e)](LICENSE)
[![platform](https://img.shields.io/badge/ESP32--S3-WT32--SC01_Plus-003c7e?style=flat-square&logo=espressif&logoColor=ffd200)](https://docs.wireless-tag.com/WT32-SC01-Plus.html)

</div>

---

## Overview

`moshon-timetable` is open-source firmware for the Wireless-Tag WT32-SC01 Plus. It turns the board into a standalone NMBS / SNCB platform-style display, drawing data from the public [iRail](https://docs.irail.be/) API. Departures, arrivals, delays, cancellations and intermediate stops are rendered in the same visual style used in Belgian stations.

The device is configured once over a built-in captive portal and runs unattended after that. There is no companion app and no account.

## Table of contents

- [Features](#features)
- [Quick start](#quick-start)
- [The display](#the-display)
- [Touch controls](#touch-controls)
- [Hardware](#hardware)
- [Build from source](#build-from-source)
- [Continuous integration](#continuous-integration)
- [License and credits](#license-and-credits)

---

## Features

- **Live departures and arrivals** for any of the 714 NMBS / SNCB stations, refreshed every 45 seconds.
- **Real-time delays and cancellations**, displayed with a red `+N'` badge and a revised time, or a grey strikethrough for cancelled trains.
- **Synchronised via-stop cycling** across all rows, so multiple long itineraries are readable without clutter.
- **Per-train fonts**: the destination name auto-shrinks to fit the column, so station names are never truncated mid-word.
- **Multilingual user interface** in Dutch, French, English or German, selected during first-time setup.
- **Data-freshness indicator** in the header: a small dot is green when the data is under one minute old, yellow up to two minutes, and red beyond that or after a failed fetch.
- **Captive-portal setup**: no firmware rebuild is required to change Wi-Fi or station — long-press the screen for three seconds to start over.

---

## Quick start

The fastest way to bring a device up. Requires only a USB cable and Python.

### 1. Flash the prebuilt firmware

1. Download `moshon_timetable-full.bin` from the [latest release](https://github.com/matsvandamme/moshon-timetable/releases/latest). The asset is a single image containing the bootloader, partition table and application.
2. Plug the board into your computer over USB. It enumerates as a CDC serial port.
3. Flash it with [esptool](https://docs.espressif.com/projects/esptool/):

   ```sh
   pip install esptool
   esptool.py --chip esp32s3 --port COM11 write_flash 0x0 moshon_timetable-full.bin
   ```

   Replace `COM11` with your serial port (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux).

### 2. Configure Wi-Fi and station

On first boot, or after a long-press reset, the device displays:

> **moshon-timetable setup**
> Connect to Wi-Fi: `moshon-setup`
> Then visit: `http://192.168.4.1`

1. On a phone or laptop, join the open `moshon-setup` access point that the device advertises.
2. Open `http://192.168.4.1` in a browser. Most operating systems open the page automatically via captive-portal detection.
3. Pick your display language, enter your home Wi-Fi credentials, and choose a station from the searchable list of all 714 NMBS / SNCB stations.
4. Submit the form. The device reboots and joins your network.

Subsequent boots go straight to the live timetable.

---

## The display

```
┌───────────────────────────────────────────────────────────────┐
│  12:34  Departures                       ●  [B]   Aalter      │
├───────────────────────────────────────────────────────────────┤
│  12:38  Brussels-South                                        │
│  +3'    via Ghent-Sint-Pieters, Denderleeuw          1   IC   │
│  12:41                                                  2204  │
├───────────────────────────────────────────────────────────────┤
│  12:42  Ostend                                                │
│         via Bruges                                   2   IC   │
│                                                         2823  │
└───────────────────────────────────────────────────────────────┘
```

| Element | Meaning |
|:--|:--|
| Yellow station name | Destination (or origin, in arrivals view). |
| White scheduled time | The planned departure or arrival time. |
| Red `+N'` badge | Delay in minutes, with the revised time displayed below. |
| Grey strikethrough name | The train has been cancelled. |
| `via …` subtitle | Intermediate stops. Long lists page through every few seconds. |
| White outlined number | Platform number. |
| Service-class label | Class (IC, S, L, …) above the train number (e.g. 2204). |
| Header dot | Data freshness: green ≤ 60 s, yellow 60–120 s, red > 120 s or fetch failed. |

When a destination name exceeds the column width, the font scales down for that row rather than truncating.

---

## Touch controls

| Gesture | Effect |
|:--|:--|
| Single tap anywhere on the screen | Toggle between Departures and Arrivals. |
| Single tap on the NMBS logo | Same as above. |
| Press and hold for three seconds | Clear stored Wi-Fi and station settings, then reboot into the setup portal. |

---

## Hardware

| Component | Detail |
|:--|:--|
| Board | [Wireless-Tag WT32-SC01 Plus](https://www.wireless-tag.com/portfolio/wt32-sc01plus/) (also sold as Seeed Studio SKU `BH2306250001`) |
| MCU | ESP32-S3 (WT32-S3-WROVER module), 16 MB flash, 2 MB quad PSRAM |
| Display | 3.5″ IPS, 480 × 320, ST7796 controller over 8-bit i80 parallel |
| Touch | FT6336U capacitive controller over I²C |
| USB | Native USB-CDC (VID `0x303A` / PID `0x1001`) |
| Power | USB-C, 500 mA is sufficient |

---

## Build from source

Required only if you intend to modify the firmware.

<details>
<summary>Click to expand build instructions</summary>

### Prerequisites

- [ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/index.html), configured for the `esp32s3` target. On Windows, the official Espressif installer is the recommended path. On Linux or macOS:

  ```sh
  mkdir -p ~/esp && cd ~/esp
  git clone --depth=1 --branch v5.3.2 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32s3
  . ./export.sh
  ```

- Python 3.9 or newer on `PATH`.

### Build, flash, monitor

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

`Ctrl+]` exits the serial monitor. The first build downloads several hundred megabytes of managed components (LVGL, `esp_lcd_st7796`, `esp_lcd_touch_ft5x06`); subsequent builds are incremental.

### Configuration

Run `idf.py menuconfig` and open the **Moshon Timetable** submenu.

| Option | Default | Description |
|:--|:--|:--|
| Active NMBS station | `Aalter` | Compile-time fallback. Overridden by the value saved through the setup portal. |
| Refresh period | `45 s` | Polling interval for the iRail liveboard. Values below 30 s are discouraged; iRail is run by volunteers and publishes a rate limit. |
| Demo mode | off | Disables Wi-Fi and iRail and renders synthetic data. Useful for screenshots and offline development. |

</details>

---

## Continuous integration

- **Per-commit builds**: GitHub Actions runs `espressif/esp-idf-ci-action` on every push and pull request. Artifacts are retained for 30 days.
- **Tagged releases**: pushing a `v*` tag triggers a build that merges the bootloader, partition table and application into a single `moshon_timetable-full.bin`, then publishes a GitHub Release with auto-generated notes.

No secrets are required at build time. The firmware obtains Wi-Fi credentials at runtime through the captive-portal setup flow.

---

## License and credits

Released under the [MIT License](LICENSE). Trademark notices and third-party component licences are documented in [NOTICE.md](NOTICE.md).

Live train data is provided by the [iRail](https://docs.irail.be/) project, which is run by volunteers. The firmware ships with conservative defaults that stay within their published rate limits.

Maintained by [Matthieu Van Damme](https://github.com/matsvandamme) under the [VMAT Contracting](https://vmat-contracting.be) banner.
