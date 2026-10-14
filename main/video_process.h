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
    uint16_t source_frame_count;
    uint16_t frame_count;
    uint16_t preview_frame_count;
    uint16_t last_frame;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t width;
    uint16_t height;
    uint16_t preview_width;
    uint16_t preview_height;
    uint32_t frame_interval_ms;
    uint32_t preview_interval_ms;
    uint32_t last_process_ms;
    uint32_t last_frame_ms;
    size_t output_bytes;
    size_t preview_bytes;
    size_t free_psram;
} prisma_video_process_status_t;

esp_err_t prisma_video_process_init(void);

/* One-shot preprocessing. Reads source VIDEO A/B from flash, creates cached
 * VIDEO OUT + PREVIEW in their dedicated flash slots, then releases all
 * temporary PSRAM before returning. */
esp_err_t prisma_video_process_prepare(void);

esp_err_t prisma_video_process_get_status(prisma_video_process_status_t *out);

/* Cached playback only: these never decode the original MJPEG source. */
esp_err_t prisma_video_process_send_indexed(httpd_req_t *req,
                                            uint16_t index,
                                            bool preview);
esp_err_t prisma_video_process_send_bmp(httpd_req_t *req,
                                        uint16_t index,
                                        bool preview);

/* Render a cached frame from flash to an RGB565 target buffer. Intended for
 * appliance playback from PREVIEW. Temporary frame memory is released before
 * this function returns; no persistent PSRAM frame buffer is retained. */
esp_err_t prisma_video_process_copy_cached_rgb565(uint16_t index,
                                                  bool preview,
                                                  uint16_t *dst,
                                                  uint16_t dst_width,
                                                  uint16_t dst_height,
                                                  bool byte_swap,
                                                  uint16_t *src_width,
                                                  uint16_t *src_height);
