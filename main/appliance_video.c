#include "appliance_results.h"

#include "video_process.h"

esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count,
                                         uint32_t *frame_interval_ms)
{
    prisma_video_process_status_t st = {0};
    esp_err_t err = prisma_video_process_get_status(&st);
    if (err != ESP_OK) return err;
    if (!st.prepared || !st.preview_frame_count) return ESP_ERR_NOT_FOUND;
    if (frame_count) *frame_count = st.preview_frame_count;
    if (frame_interval_ms) {
        *frame_interval_ms = st.preview_interval_ms ? st.preview_interval_ms : 200u;
    }
    return ESP_OK;
}

esp_err_t prisma_appliance_render_video_frame(uint16_t *dst,
                                              uint16_t dst_width,
                                              uint16_t dst_height,
                                              uint16_t frame_index,
                                              uint16_t *frame_count,
                                              uint32_t *frame_interval_ms)
{
    if (!dst || !dst_width || !dst_height) return ESP_ERR_INVALID_ARG;

    prisma_video_process_status_t st = {0};
    esp_err_t err = prisma_video_process_get_status(&st);
    if (err != ESP_OK) return err;
    if (!st.prepared || !st.preview_frame_count) return ESP_ERR_NOT_FOUND;

    uint16_t index = (uint16_t)(frame_index % st.preview_frame_count);
    err = prisma_video_process_copy_cached_rgb565(index,
                                                  true,
                                                  dst,
                                                  dst_width,
                                                  dst_height,
                                                  true,
                                                  NULL,
                                                  NULL);
    if (err != ESP_OK) return err;

    if (frame_count) *frame_count = st.preview_frame_count;
    if (frame_interval_ms) {
        *frame_interval_ms = st.preview_interval_ms ? st.preview_interval_ms : 200u;
    }
    return ESP_OK;
}
