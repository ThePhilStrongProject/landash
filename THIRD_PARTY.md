# Third-party software and data

LANDA.SH is built from the following. Components fetched by the ESP-IDF
component manager are pinned in `dependencies.lock` and downloaded at build
time; they are not stored in this repository.

## Components

| Component | Version | Licence | Used for |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | 5.5 | Apache-2.0 | The SDK: FreeRTOS, lwIP, Wi-Fi, HTTP server and client, NVS, OTA, SPIFFS |
| ↳ [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (in ESP-IDF) | | Apache-2.0 | HTTPS for updates; AES-256-GCM and PBKDF2 for the credential vault |
| ↳ [cJSON](https://github.com/DaveGamble/cJSON) (in ESP-IDF) | | MIT | Every JSON request and response |
| ↳ [lwIP](https://savannah.nongnu.org/projects/lwip/) (in ESP-IDF) | | BSD-3-Clause | The TCP/IP stack |
| ↳ [FreeRTOS](https://www.freertos.org) (in ESP-IDF) | | MIT | Tasks and synchronisation |
| [LVGL](https://github.com/lvgl/lvgl) | 9.6.0 | MIT | The dongle's screen |
| ↳ Montserrat font (bundled with LVGL) | | SIL Open Font License 1.1 | Text on the screen |
| [espressif/esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port) | 2.9.0 | Apache-2.0 | Connecting LVGL to the display driver |
| [espressif/mdns](https://components.espressif.com/components/espressif/mdns) | 1.13.1 | Apache-2.0 | Announcing `landash.local` and discovering device names |
| [espressif/button](https://components.espressif.com/components/espressif/button) | 4.2.1 | Apache-2.0 | The BOOT button |
| [espressif/cmake_utilities](https://components.espressif.com/components/espressif/cmake_utilities) | 1.1.1 | Apache-2.0 | Build support for the button component |

The full licence texts are included with each component once it has been
downloaded (`managed_components/`) and in the ESP-IDF source tree.

## Data

| Data | Source | Used for |
|---|---|---|
| MAC address block assignments (`main/net/oui_table.inc`) | The IEEE Registration Authority's public MA-L listing, processed by `tools/gen_oui.py` | Naming the maker of each device |
| Time zone rules (`TZ_POSIX` in `main/web/www/index.html`) | [posix_tz_db](https://github.com/nayarsystems/posix_tz_db) (MIT), from the IANA time zone database; quoted zone names replaced with plain placeholders | Setting the dongle's clock from the browser's time zone |

## Services contacted at run time

These are not bundled, but the dongle talks to them. [SECURITY.md](SECURITY.md)
has the details.

- `pool.ntp.org`, for the time.
- `raw.githubusercontent.com`, for update checks and downloads.
- `1.1.1.1` and a DNS lookup of `example.com`, for internet health checks
  (both configurable).
