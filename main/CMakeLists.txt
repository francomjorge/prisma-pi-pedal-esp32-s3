idf_component_register(
    SRCS "app36.c" "wifi_portal.c" "api_core3a.c" "diagnostics.c" "discovery.c" "discovery_udp.c" "prisma_touch.c" "storage.c" "image_ab.c" "video_ab.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd esp_partition spiffs json mdns lwip esp_timer
    PRIV_REQUIRES esp_app_format
    EMBED_TXTFILES "web/index.html" "web/style.css"
)

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=httpd_start")

spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)
