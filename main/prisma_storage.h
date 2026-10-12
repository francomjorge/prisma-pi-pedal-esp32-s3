#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PRISMA_MEDIA_IMAGE = 1,
    PRISMA_MEDIA_VIDEO = 2,
} prisma_media_kind_t;

typedef struct {
    bool image_valid;
    bool video_valid;
    char active_image_slot;
    char active_video_slot;
    uint32_t image_generation;
    uint32_t video_generation;
} prisma_storage_status_t;

bool prisma_storage_init(prisma_storage_status_t *status);
