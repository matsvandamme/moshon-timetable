<div align="center">

# moshon-timetable

### A real NMBS departure board for your desk.

[![build](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml/badge.svg)](https://github.com/matsvandamme/moshon-timetable/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/matsvandamme/moshon-timetable?style=flat-square&color=ffd200&labelColor=003c7e&label=release)](https://github.com/matsvandamme/moshon-timetable/releases/latest)
[![flashable bin](https://img.shields.io/badge/download-moshon__timetable--full.bin-003c7e?style=flat-square&logo=espressif&logoColor=white)](https://github.com/matsvandamme/moshon-timetable/releases/latest)
[![license](https://img.shields.io/github/license/matsvandamme/moshon-timetable?style=flat-square&color=ffd200&labelColor=003c7e)](LICENSE)
[![platform](https://img.shields.io/badge/ESP32--S3-WT32--SC01_Plus-003c7e?style=flat-square&logo=espressif&logoColor=ffd200)](https://docs.wireless-tag.com/WT32-SC01-Plus.html)

**Live train departures and arrivals for any Belgian station, on a 3.5" colour touchscreen.**
Powered by the open [iRail](https://docs.irail.be/) API.

</div>

---

## What it does

It looks like the boards you stare at on the platform — and it works the same way.

- **Live Vertrek / Aankomst** for any of the 714 Belgian stations, refreshed every 45 s.
- **Real-time delays and cancellations** shown the way NMBS itself shows them — red `+5'` badge with the revised time, grey strikethrough for cancelled trains.
- **Via-stops cycle** through the intermediate calls of every train, in lockstep across the whole board.
- **Tap anywhere on the screen** to flip between departures and arrivals.
- **No PC, no phone, no app required after first-time setup** — it's a self-contained appliance.

<div align="center">

| | |
|:-:|:-:|
| **Vertrek** (departures) | **Aankomst** (arrivals) |
| live times, delays, via-stops, platforms | same data, other direction |

</div>

---

## Setup in under a minute

You don't need a toolchain. You don't need to compile anything. Just flash and turn it on.

### 1. Get the firmware on the board

Download **`moshon_timetable-full.bin`** from the [latest release](https://github.com/matsvandamme/moshon-timetable/releases/latest).
This is a single image — bootloader, partition table and app, already merged.

Then plug the board into USB and flash:

```sh
pip install esptool
esptool.py --chip esp32s3 --port COM11 write_flash 0x0 moshon_timetable-full.bin
```

> Replace `COM11` with your port name. On macOS it's usually `/dev/cu.usbmodem*`, on Linux `/dev/ttyACM0`.

### 2. Connect it to your Wi-Fi

The first time it boots — or any time you reset it — the screen will say:

> **moshon-timetable setup**
> Connect to Wi-Fi: `moshon-setup`
> Then visit: `http://192.168.4.1`

1. On your phone or laptop, join the **`moshon-setup`** Wi-Fi network the board is advertising.
2. Open `http://192.168.4.1` in any browser.
3. Type in your home Wi-Fi name and password, and pick your station from the searchable list (all 714 NMBS stations, in Dutch).
4. Press save. The board reboots and joins your network.

That's it. From this point on it boots straight into the live timetable.

### 3. Touch controls

| Gesture | Effect |
|:--|:--|
| **Tap anywhere on the screen** | Switch between **Vertrek** and **Aankomst** |
| **Tap the NMBS logo** | Same thing |
| **Press & hold for 3 seconds** | Wipe Wi-Fi + station, fall back into setup mode |

---

## What's on the board

```
┌───────────────────────────────────────────────────────────────┐
│  12:34  Vertrek                              [B]   Aalter     │ ← clock, mode, station
├───────────────────────────────────────────────────────────────┤
│  12:38  Brussel-Zuid                                          │
│  +3'    via Gent-Sint-Pieters, Denderleeuw           1   IC   │
│  12:41                                                  2204  │
├───────────────────────────────────────────────────────────────┤
│  12:42  Oostende                                              │
│         via Brugge                                   2   IC   │
│                                                         2823  │
└───────────────────────────────────────────────────────────────┘
```

| Element | What it tells you |
|:--|:--|
| **Yellow station name** | Where this train is going (or where it came from, in arrivals view) |
| **White time (top-left)** | Scheduled time |
| **Red `+N'` badge** | Delay in minutes, with the new departure/arrival time below it |
| **Grey strikethrough name** | Train is cancelled |
| **`via …` subtitle** | Intermediate stops — cycles through if there are more than fit on one line |
| **White outlined number** | Platform |
| **Bold `IC` / `S` / `L`** | Service class. The smaller number underneath (e.g. `2204`) is the train number |

If a destination is too long for one line, the font shrinks automatically so the whole name stays readable. Nothing ever gets cut off as "Brus…".

---

## Hardware

You need one device:

<div align="center">

[![WT32-SC01 Plus](https://img.shields.io/badge/Wireless--Tag-WT32--SC01_Plus-003c7e?style=for-the-badge&logo=espressif&logoColor=ffd200)](https://www.wireless-tag.com/portfolio/wt32-sc01plus/)

</div>

| | |
|:--|:--|
| **MCU** | ESP32-S3 (WT32-S3-WROVER module) — 16 MB flash, 2 MB quad PSRAM |
| **Display** | 3.5" IPS 480×320, ST7796 over 8-bit i80 parallel |
| **Touch** | FT6336U capacitive, over I²C |
| **USB** | Native USB-CDC (VID `0x303A` / PID `0x1001`) — appears as a COM port |
| **Power** | Any USB-C cable, 500 mA is plenty |

Also sold as **Seeed Studio SKU BH2306250001**.

---

## Build from source

You only need this if you want to hack on the firmware itself.

<details>
<summary><b>Click to expand build instructions</b></summary>

### Prerequisites

- **[ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/index.html)** for the `esp32s3` target.
  - Windows: use the [official Espressif installer](https://dl.espressif.com/dl/esp-idf/) — easiest path.
  - Linux/macOS:
    ```sh
    mkdir -p ~/esp && cd ~/esp
    git clone --depth=1 --branch v5.3.2 --recursive https://github.com/espressif/esp-idf.git
    cd esp-idf && ./install.sh esp32s3
    . ./export.sh
    ```
- Python ≥ 3.9 on `PATH` (Windows: install from python.org or via `winget install Python.Python.3.12`).

### Build & flash

```sh
. $IDF_PATH/export.sh           # activates idf.py
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

`Ctrl+]` exits the serial monitor.

The first build pulls a few hundred MB of managed components (LVGL, `esp_lcd_st7796`, `esp_lcd_touch_ft5x06`) — give it a minute. Subsequent builds are incremental.

### Configure (optional)

```sh
idf.py menuconfig
```

Under **Moshon Timetable**:

| Option | Default | What it does |
|:--|:--|:--|
| Active NMBS station | `Aalter` | Compile-time default station. NVS overrides this once the user picks one in the setup portal. |
| Refresh period | `45 s` | How often to poll iRail. Don't go below ~30 s — iRail is volunteer-run. |
| Demo mode | off | Skips Wi-Fi/iRail and renders synthetic data — useful for screenshots. |

</details>

---

## Continuous integration

- **Every push** → CI builds the firmware via `espressif/esp-idf-ci-action` and uploads the binaries as artifacts (30-day retention).
- **Every `v*` tag** → CI builds, merges into a single flashable image with `esptool merge_bin`, and publishes a GitHub Release with auto-generated notes.

No secrets are needed — the firmware uses runtime AP-mode provisioning, not compile-time credentials.

---

## Why "moshon"

A nod to the **Mosin** rifle's elegance under everyday conditions — same energy: a single tool that does one job well, year after year, with no maintenance.

---

## Credits & licence

Built by **[Matthieu Van Damme](https://github.com/matsvandamme)** under the [VMAT Contracting](https://vmat-contracting.be) flag.
Live data courtesy of the **[iRail](https://docs.irail.be/)** project — a free, open API run by volunteers. Please don't abuse it.

Released under the **[MIT License](LICENSE)**. Third-party components keep their own licences (ESP-IDF: Apache 2.0; LVGL: MIT).

### Trademark notice

The NMBS / SNCB **B**-in-oval mark (designed by Henry van de Velde in 1936) is a registered trademark of NMBS / SNCB. It is embedded in this firmware solely to identify the operator whose live data the device displays — the canonical fair-use scenario. If you redistribute the firmware in modified form, please continue to use it only for identification of NMBS / SNCB train information.

<div align="center">

---

**If this thing ends up on someone's desk, send a photo.** That's the whole reward loop.

</div>
