idf_component_register(
    SRCS "app36.c" "wifi_portal.c" "network_ui.c" "api_core3a.c" "admin_auth.c" "admin_api.c" "admin_wifi_harden.c" "storage_verify_api.c" "process_api.c" "poster_config.c" "processed_store.c" "local_process.c" "video_process_api.c" "video_process.c" "diagnostics.c" "discovery.c" "discovery_udp.c" "prisma_touch.c" "image_ab.c" "video_ab.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd esp_partition json mdns lwip esp_timer esp_jpeg mbedtls
    PRIV_REQUIRES esp_app_format
    EMBED_TXTFILES "web/index.html" "web/style.css" "web/admin.html" "web/process.html"
)

# Keep the complete existing admin implementation, but rename only its exported
# route registrar. admin_wifi_harden.c calls it and then replaces Wi-Fi connect.
set_source_files_properties(admin_api.c PROPERTIES
    COMPILE_DEFINITIONS "prisma_admin_register_routes=prisma_admin_register_routes_legacy"
)

# Compact rendering loops are intentional. Keep real compiler errors fatal while
# not promoting formatting or one harmless capacity-bookkeeping variable to errors.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=misleading-indentation
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=httpd_start")
