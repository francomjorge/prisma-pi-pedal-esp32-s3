#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PRISMA_POSTER_PRESET_AUTO = 0,
    PRISMA_POSTER_PRESET_720 = 720,
    PRISMA_POSTER_PRESET_480 = 480,
    PRISMA_POSTER_PRESET_360 = 360,
    PRISMA_POSTER_PRESET_240 = 240,
    PRISMA_POSTER_PRESET_166 = 166,
} prisma_poster_preset_t;

typedef enum {
    PRISMA_POSTER_ASPECT_SOURCE = 0,
    PRISMA_POSTER_ASPECT_16_9 = 1,
    PRISMA_POSTER_ASPECT_9_16 = 2,
    PRISMA_POSTER_ASPECT_4_3 = 3,
    PRISMA_POSTER_ASPECT_3_4 = 4,
    PRISMA_POSTER_ASPECT_1_1 = 5,
} prisma_poster_aspect_t;

typedef struct {
    prisma_poster_preset_t image_preset;
    prisma_poster_aspect_t image_aspect;
    prisma_poster_preset_t video_preset;
    prisma_poster_aspect_t video_aspect;
} prisma_poster_config_t;

void prisma_poster_config_defaults(prisma_poster_config_t *out);
esp_err_t prisma_poster_config_load(prisma_poster_config_t *out);
esp_err_t prisma_poster_config_save(const prisma_poster_config_t *cfg);
bool prisma_poster_config_valid(const prisma_poster_config_t *cfg);

bool prisma_poster_resolve_output(prisma_poster_preset_t preset,
                                  prisma_poster_aspect_t aspect,
                                  uint16_t source_width,
                                  uint16_t source_height,
                                  uint16_t *out_width,
                                  uint16_t *out_height);
uint32_t prisma_poster_config_hash(const prisma_poster_config_t *cfg, bool video);
