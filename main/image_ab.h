#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define PRISMA_IMAGE_MAX_BYTES (1024u * 1024u)

typedef struct {
    bool ready;
    bool valid;
    char active_bank;
    uint32_t generation;
    size_t size;
    uint32_t crc32;
} prisma_image_info_t;

esp_err_t prisma_image_ab_init(void);
esp_err_t prisma_image_ab_get_info(prisma_image_info_t *out);
esp_err_t prisma_image_ab_begin(size_t expected_size);
esp_err_t prisma_image_ab_write(size_t offset, const void *data, size_t len);
esp_err_t prisma_image_ab_finalize(void);
void prisma_image_ab_abort(void);
esp_err_t prisma_image_ab_delete(void);
esp_err_t prisma_image_ab_read(size_t offset, void *dst, size_t len);
