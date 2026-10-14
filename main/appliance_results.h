#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t prisma_appliance_render_image_result(uint16_t *dst,
                                               uint16_t dst_width,
                                               uint16_t dst_height,
                                               uint16_t *source_width,
                                               uint16_t *source_height);

esp_err_t prisma_appliance_render_generator_result(uint16_t *dst,
                                                   uint16_t dst_width,
                                                   uint16_t dst_height,
                                                   char *generator_name,
                                                   size_t generator_name_size);

/* VIDEO appliance page is playback-only. It never triggers MJPEG decoding;
 * it opens the preprocessed PREVIEW cache and maps logical source frames onto
 * cached preview frames so the original 5 fps duration is preserved. */
esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count);
esp_err_t prisma_appliance_render_video_frame(uint16_t *dst,
                                              uint16_t dst_width,
                                              uint16_t dst_height,
                                              uint16_t frame_index,
                                              uint16_t *frame_count,
                                              uint32_t *process_ms);
