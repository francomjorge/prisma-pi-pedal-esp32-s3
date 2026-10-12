idf_component_register(
    SRCS "app36.c" "wifi_portal.c" "prisma_touch.c" "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd spiffs json
    EMBED_TXTFILES "web/index.html" "web/style.css"
)

spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)
