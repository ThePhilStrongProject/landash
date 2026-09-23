# NetDash main component.
#
# Sources are globbed so that parallel work packages never have to edit a SRCS
# list. Add a .c file anywhere under main/ and re-run cmake (idf.py build does
# this automatically when this file's timestamp changes; use `idf.py reconfigure`
# if a newly added file is not picked up).

file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "."
        "db"
        "net"
        "settings"
        "ui"
        "web"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        esp_http_client
        json
        lwip
        esp_lcd
        esp_driver_gpio
        esp_driver_spi
        esp_driver_ledc
        # The web installer's restore, over the console's USB Serial/JTAG.
        esp_driver_usb_serial_jtag
        esp_timer
        driver
        # AES-GCM and PBKDF2 for the secret vault in db/notes.c.
        mbedtls
        # The storage partition, where uploaded dashboard icons live.
        spiffs
        app_update
        esp_app_format
        # Firmware updates in net/ota.c: esp_http_client over TLS, esp_ota_*.
        esp-tls
        espressif__mdns
        lvgl__lvgl
        espressif__esp_lvgl_port
        espressif__button
)

# ---------------------------------------------------------------------------
# Embed the web UI, gzipped at build time.
#
# main/web/www/index.html -> ${CMAKE_BINARY_DIR}/index.html.gz -> .rodata
# Symbols: _binary_index_html_gz_start / _binary_index_html_gz_end
# ---------------------------------------------------------------------------
idf_build_get_property(python PYTHON)

set(netdash_www_src "${CMAKE_CURRENT_SOURCE_DIR}/web/www/index.html")
set(netdash_www_copy "${CMAKE_BINARY_DIR}/index.html")
set(netdash_www_gz "${CMAKE_BINARY_DIR}/index.html.gz")

# `python -m gzip <file>` writes <file>.gz next to its input, so the source is
# copied into the build directory first to keep the source tree clean.
add_custom_command(
    OUTPUT "${netdash_www_gz}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${netdash_www_src}" "${netdash_www_copy}"
    COMMAND ${python} -m gzip --best "${netdash_www_copy}"
    DEPENDS "${netdash_www_src}"
    COMMENT "Gzipping web UI (index.html -> index.html.gz)"
    VERBATIM
)
add_custom_target(netdash_www_gz_target DEPENDS "${netdash_www_gz}")
add_dependencies(${COMPONENT_LIB} netdash_www_gz_target)

target_add_binary_data(${COMPONENT_LIB} "${netdash_www_gz}" BINARY DEPENDS "${netdash_www_gz}")

# The Cyberpunk theme's typefaces, served as they are at /fonts/ (woff2 is
# already compressed). Symbols: _binary_<name with - and . as _>_start / _end.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/web/www/fonts/share-tech-mono.woff2" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/web/www/fonts/orbitron-700.woff2" BINARY)
