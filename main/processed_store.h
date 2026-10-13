#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PRISMA_PROCESSED_IMAGE_OUT = 0,
    PRISMA_PROCESSED_VIDEO_OUT = 1,
    PRISMA_PROCESSED_IMAGE_PREVIEW = 2,
    PRISMA_PROCESSED_VIDEO_PREVIEW = 3,
    PRISMA_PROCESSED_SLOT_COUNT = 4,
} prisma_processed_slot_t;

typedef enum {
    PRISMA_PROCESSED_FMT_INDEX8 = 1,
    PRISMA_PROCESSED_FMT_VIDEO_INDEX8 = 2,
} prisma_processed_format_t;

typedef struct {
    bool ready;
    bool valid;
    prisma_processed_slot_t slot;
    prisma_processed_format_t format;
    uint32_t source_generation;
    uint32_t config_hash;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t source_frame_count;
    uint32_t frame_interval_ms;
    size_t payload_size;
    size_t payload_capacity;
    uint32_t payload_crc32;
} prisma_processed_info_t;

esp_err_t prisma_processed_store_init(void);
size_t prisma_processed_capacity(prisma_processed_slot_t slot);
esp_err_t prisma_processed_get_info(prisma_processed_slot_t slot, prisma_processed_info_t *out);
esp_err_t prisma_processed_begin(prisma_processed_slot_t slot,
                                 prisma_processed_format_t format,
                                 uint32_t source_generation,
                                 uint32_t config_hash,
                                 uint16_t width,
                                 uint16_t height);
esp_err_t prisma_processed_write(prisma_processed_slot_t slot, size_t offset,
                                 const void *data, size_t len);
esp_err_t prisma_processed_finalize(prisma_processed_slot_t slot,
                                    uint16_t frame_count,
                                    uint16_t source_frame_count,
                                    uint32_t frame_interval_ms);
void prisma_processed_abort(prisma_processed_slot_t slot);
esp_err_t prisma_processed_invalidate(prisma_processed_slot_t slot);
esp_err_t prisma_processed_read(prisma_processed_slot_t slot, size_t offset,
                                void *dst, size_t len);
