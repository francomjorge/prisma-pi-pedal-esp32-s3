idf_component_register(
    SRCS
        "main.c"
        "prisma_display.c"
        "prisma_touch.c"
        "prisma_power.c"
        "prisma_storage.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_lcd
        esp_partition
)
