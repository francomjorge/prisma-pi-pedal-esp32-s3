#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PRISMA_POSTER_IMAGE_SOURCE = 0,
    PRISMA_POSTER_IMAGE_MANUAL = 1,
} prisma_poster_image_mode_t;

typedef struct {
    prisma_poster_image_mode_t image_mode;
    uint16_t image_width;
    uint16_t image_height;
    uint16_t video_width;
    uint16_t video_height;
} prisma_poster_config_t;

void prisma_poster_config_defaults(prisma_poster_config_t *out);
esp_err_t prisma_poster_config_load(prisma_poster_config_t *out);
esp_err_t prisma_poster_config_save(const prisma_poster_config_t *cfg);
bool prisma_poster_config_valid(const prisma_poster_config_t *cfg);
