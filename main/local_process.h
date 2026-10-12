#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    bool ready;
    bool busy;
    bool valid;
    char last_test[16];
    char error[96];
    uint16_t width;
    uint16_t height;
    uint32_t elapsed_ms;
    uint32_t source_generation;
    size_t pixel_bytes;
    size_t free_psram;
    uint8_t jpeg_scale_div;
} prisma_process_status_t;

esp_err_t prisma_process_init(void);
esp_err_t prisma_process_run_poster(void);
esp_err_t prisma_process_run_lines(void);
esp_err_t prisma_process_run_rectangles(void);
esp_err_t prisma_process_run_circles(void);
esp_err_t prisma_process_run_triangles(void);
esp_err_t prisma_process_get_status(prisma_process_status_t *out);
esp_err_t prisma_process_send_bmp(httpd_req_t *req);
