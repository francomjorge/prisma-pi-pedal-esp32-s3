idf_component_register(
    SRCS "app36.c" "wifi_portal.c" "wifi_guard.c" "network_ui.c" "api_core3a.c" "admin_auth.c" "admin_api.c" "storage_verify_api.c" "process_api.c" "poster_config.c" "processed_store.c" "local_process.c" "jpeg_adaptive_wrap.c" "video_process_api.c" "video_process.c" "appliance_results.c" "appliance_video.c" "diagnostics.c" "discovery.c" "discovery_udp.c" "prisma_touch.c" "image_ab.c" "video_ab.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd esp_partition json mdns lwip esp_timer esp_jpeg mbedtls
    PRIV_REQUIRES esp_app_format
    EMBED_TXTFILES "web/index.html" "web/style.css" "web/admin.html" "web/process.html"
)

# Compact rendering loops are intentional. Keep real compiler errors fatal while
# not promoting formatting or one harmless capacity-bookkeeping variable to errors.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=misleading-indentation
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=httpd_start"
    "-Wl,--wrap=httpd_register_uri_handler"
    "-Wl,--wrap=prisma_process_run_poster"
    "-Wl,--wrap=prisma_process_get_status"
    "-Wl,--wrap=prisma_process_send_bmp"
    "-Wl,--wrap=prisma_process_run_lines"
    "-Wl,--wrap=prisma_process_run_rectangles"
    "-Wl,--wrap=prisma_process_run_circles"
    "-Wl,--wrap=prisma_process_run_triangles"
)
