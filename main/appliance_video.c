#include "appliance_results.h"

#include "video_process.h"

esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count)
{
    esp_err_t err = prisma_video_process_prepare();
    if (err != ESP_OK) return err;

    prisma_video_process_status_t st = {0};
    err = prisma_video_process_get_status(&st);
    if (err != ESP_OK) return err;
    if (!st.prepared || !st.frame_count) return ESP_ERR_NOT_FOUND;
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

    prisma_video_process_status_t st = {0};
    esp_err_t err = prisma_video_process_get_status(&st);
    if (err != ESP_OK) return err;
    if (!st.prepared || !st.frame_count) {
        err = prisma_appliance_prepare_video(NULL);
        if (err != ESP_OK) return err;
        err = prisma_video_process_get_status(&st);
        if (err != ESP_OK) return err;
    }

    uint16_t index = st.frame_count ? (uint16_t)(frame_index % st.frame_count) : 0;
    err = prisma_video_process_render_frame(index);
    if (err != ESP_OK) return err;

    err = prisma_video_process_copy_scaled_rgb565(dst,
                                                  dst_width,
                                                  dst_height,
                                                  true,
                                                  NULL,
                                                  NULL);
    if (err != ESP_OK) return err;

    if (prisma_video_process_get_status(&st) == ESP_OK) {
        if (frame_count) *frame_count = st.frame_count;
        if (process_ms) *process_ms = st.last_frame_ms;
    }
    return ESP_OK;
}
