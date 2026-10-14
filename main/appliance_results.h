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
