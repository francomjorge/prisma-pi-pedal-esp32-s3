#include "prisma_storage.h"
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

#define PRISMA_MEDIA_MAGIC 0x504D4544u
#define PRISMA_MEDIA_VERSION 1u
#define IO_CHUNK 4096u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t crc32;
    uint32_t reserved[3];
} prisma_media_header_t;

typedef struct {
    const esp_partition_t *part;
    bool valid;
    uint32_t generation;
} slot_state_t;

static const char *TAG = "PEDAL_STORAGE";

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool validate_slot(const char *label, prisma_media_kind_t kind, slot_state_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        label);
    if (!part) {
        ESP_LOGE(TAG, "%s missing", label);
        return false;
    }
    out->part = part;

    prisma_media_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "%s header read failed", label);
        return false;
    }
    if (hdr.magic != PRISMA_MEDIA_MAGIC || hdr.version != PRISMA_MEDIA_VERSION || hdr.kind != (uint16_t)kind) {
        ESP_LOGI(TAG, "%s empty/invalid header", label);
        return true;
    }
    if (hdr.payload_len == 0 || hdr.payload_len > part->size - sizeof(hdr)) {
        ESP_LOGW(TAG, "%s invalid length %lu", label, (unsigned long)hdr.payload_len);
        return true;
    }

    uint8_t buf[IO_CHUNK];
    uint32_t crc = 0;
    size_t remaining = hdr.payload_len;
    size_t offset = sizeof(hdr);
    while (remaining) {
        size_t take = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        if (esp_partition_read(part, offset, buf, take) != ESP_OK) {
            ESP_LOGW(TAG, "%s payload read failed", label);
            return true;
        }
        crc = crc32_update(crc, buf, take);
        offset += take;
        remaining -= take;
    }

    if (crc != hdr.crc32) {
        ESP_LOGW(TAG, "%s CRC mismatch expected=%08lx got=%08lx", label,
                 (unsigned long)hdr.crc32, (unsigned long)crc);
        return true;
    }

    out->valid = true;
    out->generation = hdr.generation;
    ESP_LOGI(TAG, "%s valid generation=%lu bytes=%lu", label,
             (unsigned long)hdr.generation, (unsigned long)hdr.payload_len);
    return true;
}

static void select_active(const slot_state_t *a, const slot_state_t *b,
                          bool *valid, char *slot, uint32_t *generation)
{
    *valid = false;
    *slot = '-';
    *generation = 0;
    if (a->valid && (!b->valid || a->generation >= b->generation)) {
        *valid = true;
        *slot = 'A';
        *generation = a->generation;
    } else if (b->valid) {
        *valid = true;
        *slot = 'B';
        *generation = b->generation;
    }
}

bool prisma_storage_init(prisma_storage_status_t *status)
{
    if (!status) return false;
    memset(status, 0, sizeof(*status));
    status->active_image_slot = '-';
    status->active_video_slot = '-';

    slot_state_t image_a, image_b, video_a, video_b;
    bool layout_ok = true;
    layout_ok &= validate_slot("image_a", PRISMA_MEDIA_IMAGE, &image_a);
    layout_ok &= validate_slot("image_b", PRISMA_MEDIA_IMAGE, &image_b);
    layout_ok &= validate_slot("video_a", PRISMA_MEDIA_VIDEO, &video_a);
    layout_ok &= validate_slot("video_b", PRISMA_MEDIA_VIDEO, &video_b);

    select_active(&image_a, &image_b,
                  &status->image_valid,
                  &status->active_image_slot,
                  &status->image_generation);
    select_active(&video_a, &video_b,
                  &status->video_valid,
                  &status->active_video_slot,
                  &status->video_generation);

    ESP_LOGI(TAG, "IMAGE active=%c gen=%lu valid=%d", status->active_image_slot,
             (unsigned long)status->image_generation, status->image_valid);
    ESP_LOGI(TAG, "VIDEO active=%c gen=%lu valid=%d", status->active_video_slot,
             (unsigned long)status->video_generation, status->video_valid);
    return layout_ok;
}
