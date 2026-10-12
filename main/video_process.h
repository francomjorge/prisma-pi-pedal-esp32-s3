#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    bool ready;
    bool prepared;
    bool valid;
    char error[96];
    uint32_t source_generation;
    size_t source_bytes;
    uint16_t frame_count;
    uint16_t last_frame;
    uint16_t width;
    uint16_t height;
    uint32_t last_frame_ms;
    size_t pixel_bytes;
    size_t free_psram;
} prisma_video_process_status_t;

esp_err_t prisma_video_process_init(void);
esp_err_t prisma_video_process_prepare(void);
esp_err_t prisma_video_process_render_frame(uint16_t index);
esp_err_t prisma_video_process_get_status(prisma_video_process_status_t *out);
esp_err_t prisma_video_process_send_bmp(httpd_req_t *req);
