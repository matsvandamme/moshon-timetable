# Notices

This project is licensed under the MIT License — see [LICENSE](LICENSE).
This file documents third-party trademarks and assets bundled with the
firmware, and the terms under which they are included.

## NMBS / SNCB "B in oval" trademark

The "B in oval" mark (designed by Henry van de Velde in 1936) is a
registered trademark of **NMBS / SNCB** (Belgian national railway).

It is embedded in this firmware solely to identify the operator whose
live data the device displays — the canonical fair-use scenario for a
trademark. The bundled bitmap is rasterised from the publicly available
SVG on Wikimedia Commons.

The MIT license on the source code does **not** extend to this
trademark. If you redistribute the firmware in modified form, please
continue to use the mark only for identification of NMBS / SNCB train
information.

## iRail API

Live train data comes from the [iRail](https://docs.irail.be/) public
API, run by volunteers. Please respect their rate-limit guidelines
(3 requests per second / 5 burst) and identify your fork with a
descriptive `User-Agent`. The firmware does both out of the box.

## Third-party components

| Component | License |
|:--|:--|
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache 2.0 |
| [LVGL](https://github.com/lvgl/lvgl) | MIT |
| [esp_lcd_st7796](https://components.espressif.com/components/espressif/esp_lcd_st7796) | Apache 2.0 |
| [esp_lcd_touch_ft5x06](https://components.espressif.com/components/espressif/esp_lcd_touch_ft5x06) | Apache 2.0 |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT |
