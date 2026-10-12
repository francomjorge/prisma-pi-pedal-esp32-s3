idf_component_register(
    SRCS
        "main.c"
        "prisma_display.c"
        "prisma_touch.c"
        "prisma_power.c"
        "prisma_storage.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_partition
)
