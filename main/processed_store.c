#include "processed_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#define PROCESSED_MAGIC 0x504F5554u /* POUT */
#define PROCESSED_VERSION 1u
#define HEADER_SECTOR 0x1000u
#define VERIFY_CHUNK 1024u

static const char *TAG = "PRISMA_PROCESSED";

static const char *SLOT_LABELS[PRISMA_PROCESSED_SLOT_COUNT] = {
    "image_out", "video_out", "preview_img", "preview_vid"
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t slot;
    uint8_t format;
    uint32_t source_generation;
    uint32_t config_hash;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t source_frame_count;
    uint32_t frame_interval_ms;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t header_crc32;
} processed_header_t;

typedef struct {
    bool active;
    size_t written;
    uint32_t crc_state;
    processed_header_t header;
} write_session_t;

static const esp_partition_t *g_part[PRISMA_PROCESSED_SLOT_COUNT];
static write_session_t g_write[PRISMA_PROCESSED_SLOT_COUNT];
static bool g_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static uint32_t crc32_calc(const void *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)data, len);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t header_crc(const processed_header_t *h)
{
    return crc32_calc(h, offsetof(processed_header_t, header_crc32));
}

static bool valid_slot(prisma_processed_slot_t slot)
{
    return slot >= 0 && slot < PRISMA_PROCESSED_SLOT_COUNT;
}

static bool header_valid(const processed_header_t *h, prisma_processed_slot_t slot)
{
    if (!h || !valid_slot(slot)) return false;
    if (h->magic != PROCESSED_MAGIC || h->version != PROCESSED_VERSION) return false;
    if (h->slot != (uint8_t)slot) return false;
    if (h->format != PRISMA_PROCESSED_FMT_INDEX8 &&
        h->format != PRISMA_PROCESSED_FMT_VIDEO_INDEX8) return false;
    if (!h->width || !h->height || !h->payload_size) return false;
    if (h->payload_size > prisma_processed_capacity(slot)) return false;
    return h->header_crc32 == header_crc(h);
}

static esp_err_t payload_crc(prisma_processed_slot_t slot, size_t size, uint32_t *out)
{
    if (!valid_slot(slot) || !out || !g_part[slot]) return ESP_ERR_INVALID_ARG;
    if (!size || size > prisma_processed_capacity(slot)) return ESP_ERR_INVALID_SIZE;
    uint8_t buf[VERIFY_CHUNK];
    size_t off = 0;
    uint32_t crc = 0xFFFFFFFFu;
    while (off < size) {
        size_t n = size - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        esp_err_t err = esp_partition_read(g_part[slot], HEADER_SECTOR + off, buf, n);
        if (err != ESP_OK) return err;
        crc = crc32_update(crc, buf, n);
        off += n;
    }
    *out = crc ^ 0xFFFFFFFFu;
    return ESP_OK;
}

esp_err_t prisma_processed_store_init(void)
{
    if (g_ready) return ESP_OK;
    memset(g_part, 0, sizeof(g_part));
    memset(g_write, 0, sizeof(g_write));
    for (int i = 0; i < PRISMA_PROCESSED_SLOT_COUNT; ++i) {
        g_part[i] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_ANY,
                                             SLOT_LABELS[i]);
        if (!g_part[i] || g_part[i]->size <= HEADER_SECTOR) {
            ESP_LOGE(TAG, "missing processed partition %s", SLOT_LABELS[i]);
            return ESP_ERR_NOT_FOUND;
        }
    }
    g_ready = true;
    ESP_LOGI(TAG, "processed output partitions ready");
    return ESP_OK;
}

size_t prisma_processed_capacity(prisma_processed_slot_t slot)
{
    if (!valid_slot(slot) || !g_part[slot] || g_part[slot]->size <= HEADER_SECTOR) return 0;
    return g_part[slot]->size - HEADER_SECTOR;
}

esp_err_t prisma_processed_get_info(prisma_processed_slot_t slot, prisma_processed_info_t *out)
{
    if (!out || !valid_slot(slot)) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->slot = slot;
    out->ready = g_ready;
    out->payload_capacity = prisma_processed_capacity(slot);
    if (!g_ready) return ESP_ERR_INVALID_STATE;

    processed_header_t h;
    esp_err_t err = esp_partition_read(g_part[slot], 0, &h, sizeof(h));
    if (err != ESP_OK) return err;
    if (!header_valid(&h, slot)) return ESP_OK;

    out->valid = true;
    out->format = (prisma_processed_format_t)h.format;
    out->source_generation = h.source_generation;
    out->config_hash = h.config_hash;
    out->width = h.width;
    out->height = h.height;
    out->frame_count = h.frame_count;
    out->source_frame_count = h.source_frame_count;
    out->frame_interval_ms = h.frame_interval_ms;
    out->payload_size = h.payload_size;
    out->payload_crc32 = h.payload_crc32;
    return ESP_OK;
}

esp_err_t prisma_processed_begin(prisma_processed_slot_t slot,
                                 prisma_processed_format_t format,
                                 uint32_t source_generation,
                                 uint32_t config_hash,
                                 uint16_t width,
                                 uint16_t height)
{
    if (!g_ready || !valid_slot(slot)) return ESP_ERR_INVALID_STATE;
    if (g_write[slot].active) return ESP_ERR_INVALID_STATE;
    if (!width || !height) return ESP_ERR_INVALID_ARG;
    if (format != PRISMA_PROCESSED_FMT_INDEX8 && format != PRISMA_PROCESSED_FMT_VIDEO_INDEX8)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_partition_erase_range(g_part[slot], 0, g_part[slot]->size);
    if (err != ESP_OK) return err;

    write_session_t *s = &g_write[slot];
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->crc_state = 0xFFFFFFFFu;
    s->header.magic = PROCESSED_MAGIC;
    s->header.version = PROCESSED_VERSION;
    s->header.slot = (uint8_t)slot;
    s->header.format = (uint8_t)format;
    s->header.source_generation = source_generation;
    s->header.config_hash = config_hash;
    s->header.width = width;
    s->header.height = height;
    return ESP_OK;
}

esp_err_t prisma_processed_write(prisma_processed_slot_t slot, size_t offset,
                                 const void *data, size_t len)
{
    if (!g_ready || !valid_slot(slot) || !g_write[slot].active) return ESP_ERR_INVALID_STATE;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    write_session_t *s = &g_write[slot];
    if (offset != s->written) return ESP_ERR_INVALID_ARG;
    if (offset > prisma_processed_capacity(slot) || len > prisma_processed_capacity(slot) - offset)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_partition_write(g_part[slot], HEADER_SECTOR + offset, data, len);
    if (err != ESP_OK) return err;
    s->crc_state = crc32_update(s->crc_state, (const uint8_t *)data, len);
    s->written += len;
    return ESP_OK;
}

esp_err_t prisma_processed_finalize(prisma_processed_slot_t slot,
                                    uint16_t frame_count,
                                    uint16_t source_frame_count,
                                    uint32_t frame_interval_ms)
{
    if (!g_ready || !valid_slot(slot) || !g_write[slot].active) return ESP_ERR_INVALID_STATE;
    write_session_t *s = &g_write[slot];
    if (!s->written) return ESP_ERR_INVALID_SIZE;

    uint32_t streamed = s->crc_state ^ 0xFFFFFFFFu;
    uint32_t readback = 0;
    esp_err_t err = payload_crc(slot, s->written, &readback);
    if (err != ESP_OK || streamed != readback) {
        ESP_LOGE(TAG, "%s output CRC verify failed", SLOT_LABELS[slot]);
        memset(s, 0, sizeof(*s));
        return err != ESP_OK ? err : ESP_ERR_INVALID_CRC;
    }

    s->header.frame_count = frame_count;
    s->header.source_frame_count = source_frame_count;
    s->header.frame_interval_ms = frame_interval_ms;
    s->header.payload_size = (uint32_t)s->written;
    s->header.payload_crc32 = streamed;
    s->header.header_crc32 = header_crc(&s->header);

    err = esp_partition_write(g_part[slot], 0, &s->header, sizeof(s->header));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s committed %u bytes %ux%u frames=%u",
                 SLOT_LABELS[slot], (unsigned)s->written,
                 s->header.width, s->header.height, frame_count);
    }
    memset(s, 0, sizeof(*s));
    return err;
}

void prisma_processed_abort(prisma_processed_slot_t slot)
{
    if (valid_slot(slot)) memset(&g_write[slot], 0, sizeof(g_write[slot]));
}

esp_err_t prisma_processed_invalidate(prisma_processed_slot_t slot)
{
    if (!g_ready || !valid_slot(slot)) return ESP_ERR_INVALID_STATE;
    prisma_processed_abort(slot);
    return esp_partition_erase_range(g_part[slot], 0, HEADER_SECTOR);
}

esp_err_t prisma_processed_read(prisma_processed_slot_t slot, size_t offset,
                                void *dst, size_t len)
{
    if (!g_ready || !valid_slot(slot)) return ESP_ERR_INVALID_STATE;
    if (!dst || !len) return ESP_ERR_INVALID_ARG;
    prisma_processed_info_t info;
    esp_err_t err = prisma_processed_get_info(slot, &info);
    if (err != ESP_OK) return err;
    if (!info.valid) return ESP_ERR_NOT_FOUND;
    if (offset > info.payload_size || len > info.payload_size - offset) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(g_part[slot], HEADER_SECTOR + offset, dst, len);
}
