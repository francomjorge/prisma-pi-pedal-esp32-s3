#include "appliance_results.h"

#include <string.h>

#include "stream_control.h"
#include "video_process.h"

static void blank_with_dots(uint16_t *dst, uint16_t w, uint16_t h)
{
    if (dst) memset(dst, 0, (size_t)w * h * sizeof(uint16_t));
    prisma_appliance_overlay_dots(dst, w, h, 6, 7);
}

esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count)
{
    if (!prisma_appliance_stream_enabled()) {
        if (frame_count) *frame_count = 1;
        return ESP_OK;
    }

    esp_err_t err = prisma_video_process_prepare();
    if (err != ESP_OK) {
        if (frame_count) *frame_count = 1;
        return ESP_OK;
    }

    prisma_video_process_status_t st = {0};
    err = prisma_video_process_get_status(&st);
    if (err != ESP_OK || !st.prepared || !st.frame_count) {
        if (frame_count) *frame_count = 1;
        return ESP_OK;
    }
    if (frame_count) *frame_count = st.frame_count;
    return ESP_OK;
}

esp_err_t prisma_appliance_render_video_frame(uint16_t *dst,
                                              uint16_t dst_width,
                                              uint16_t dst_height,
                                              uint16_t frame_index,
                                              uint16_t *frame_count,
                                              uint32_t *process_ms)
{
    if (!dst || !dst_width || !dst_height) return ESP_ERR_INVALID_ARG;

    if (!prisma_appliance_stream_enabled()) {
        blank_with_dots(dst, dst_width, dst_height);
        if (frame_count) *frame_count = 1;
        if (process_ms) *process_ms = 0;
        return ESP_OK;
    }

    prisma_video_process_status_t st = {0};
    esp_err_t err = prisma_video_process_get_status(&st);
    if (err != ESP_OK || !st.prepared || !st.frame_count) {
        err = prisma_video_process_prepare();
        if (err != ESP_OK) {
            blank_with_dots(dst, dst_width, dst_height);
            if (frame_count) *frame_count = 1;
            if (process_ms) *process_ms = 0;
            return ESP_OK;
        }
        err = prisma_video_process_get_status(&st);
        if (err != ESP_OK || !st.prepared || !st.frame_count) {
            blank_with_dots(dst, dst_width, dst_height);
            if (frame_count) *frame_count = 1;
            if (process_ms) *process_ms = 0;
            return ESP_OK;
        }
    }

    uint16_t index = (uint16_t)(frame_index % st.frame_count);
    err = prisma_video_process_render_frame(index);
    if (err != ESP_OK) {
        blank_with_dots(dst, dst_width, dst_height);
        return ESP_OK;
    }

    err = prisma_video_process_copy_scaled_rgb565(dst,
                                                  dst_width,
                                                  dst_height,
                                                  true,
                                                  NULL,
                                                  NULL);
    if (err != ESP_OK) {
        blank_with_dots(dst, dst_width, dst_height);
        return ESP_OK;
    }

    prisma_appliance_overlay_dots(dst, dst_width, dst_height, 6, 7);
    if (prisma_video_process_get_status(&st) == ESP_OK) {
        if (frame_count) *frame_count = st.frame_count;
        if (process_ms) *process_ms = st.last_frame_ms;
    }
    return ESP_OK;
}
