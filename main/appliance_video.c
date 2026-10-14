#include "appliance_results.h"

#include "video_process.h"

esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count)
{
    prisma_video_process_status_t st = {0};
    esp_err_t err = prisma_video_process_get_status(&st);
    if (err != ESP_OK) return err;
    if (!st.prepared || !st.preview_frame_count) return ESP_ERR_NOT_FOUND;

    uint16_t logical = st.source_frame_count ? st.source_frame_count
                                             : st.preview_frame_count;
    if (frame_count) *frame_count = logical;
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
    if (!st.prepared || !st.preview_frame_count) return ESP_ERR_NOT_FOUND;

    uint16_t logical = st.source_frame_count ? st.source_frame_count
                                             : st.preview_frame_count;
    uint16_t logical_index = logical ? (uint16_t)(frame_index % logical) : 0;
    uint16_t cached_index = (uint16_t)(((uint32_t)logical_index *
                                        st.preview_frame_count) /
                                       (logical ? logical : 1u));
    if (cached_index >= st.preview_frame_count) {
        cached_index = st.preview_frame_count - 1u;
    }

    err = prisma_video_process_copy_cached_rgb565(cached_index,
                                                  true,
                                                  dst,
                                                  dst_width,
                                                  dst_height,
                                                  true,
                                                  NULL,
                                                  NULL);
    if (err != ESP_OK) return err;

    if (frame_count) *frame_count = logical;
    if (process_ms) {
        prisma_video_process_status_t after = {0};
        if (prisma_video_process_get_status(&after) == ESP_OK)
            *process_ms = after.last_frame_ms;
        else
            *process_ms = 0;
    }
    return ESP_OK;
}
