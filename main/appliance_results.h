#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void prisma_appliance_overlay_dots(uint16_t *dst,
                                   uint16_t dst_width,
                                   uint16_t dst_height,
                                   uint8_t page_index,
                                   uint8_t page_count);

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

esp_err_t prisma_appliance_prepare_video(uint16_t *frame_count);
esp_err_t prisma_appliance_render_video_frame(uint16_t *dst,
                                              uint16_t dst_width,
                                              uint16_t dst_height,
                                              uint16_t frame_index,
                                              uint16_t *frame_count,
                                              uint32_t *process_ms);
