
esp_err_t __wrap_httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    httpd_config_t expanded = *config;
    if (expanded.max_uri_handlers < 48) expanded.max_uri_handlers = 48;

    esp_err_t err = __real_httpd_start(handle, &expanded);
    if (err != ESP_OK) return err;

    esp_err_t diag = prisma_diagnostics_init();
    if (diag != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics persistence unavailable: %s", esp_err_to_name(diag));
    }

    prisma_admin_register_routes(*handle);
    prisma_storage_verify_register_routes(*handle);
    prisma_process_register_routes(*handle);
    register_core3_routes(*handle);
    return ESP_OK;
}