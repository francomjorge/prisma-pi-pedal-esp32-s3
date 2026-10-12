#include "video_ab.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#define VIDEO_META_MAGIC   0x50564944u /* PVID */
#define VIDEO_META_VERSION 1u
#define VIDEO_BANK_A       0u
#define VIDEO_BANK_B       1u

static const char *TAG = "PRISMA_VIDEO_AB";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t generation;
    uint8_t active_bank;
    uint8_t video_valid;
    uint16_t reserved1;
    uint32_t size;
    uint32_t video_crc32;
    uint32_t meta_crc32;
} video_meta_t;

typedef struct {
    bool active;
    uint8_t bank;
    size_t expected_size;
    size_t written;
    uint32_t crc_state;
} video_write_session_t;

static const esp_partition_t *g_bank[2] = {NULL, NULL};
static nvs_handle_t g_nvs = 0;
static bool g_nvs_open = false;
static bool g_ready = false;
static video_meta_t g_meta;
static int g_meta_slot = -1;
static video_write_session_t g_write;

static uint32_t crc32_update_state(uint32_t crc, const uint8_t *data, size_t len)
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
    uint32_t state = 0xFFFFFFFFu;
    state = crc32_update_state(state, (const uint8_t *)data, len);
    return state ^ 0xFFFFFFFFu;
}

static uint32_t meta_crc(const video_meta_t *m)
{
    return crc32_calc(m, offsetof(video_meta_t, meta_crc32));
}

static bool meta_record_valid(const video_meta_t *m)
{
    if (!m) return false;
    if (m->magic != VIDEO_META_MAGIC || m->version != VIDEO_META_VERSION) return false;
    if (m->active_bank > VIDEO_BANK_B) return false;
    if (m->video_valid) {
        if (m->size == 0 || m->size > PRISMA_VIDEO_MAX_BYTES) return false;
    } else if (m->size != 0) {
        return false;
    }
    return m->meta_crc32 == meta_crc(m);
}

static bool generation_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static esp_err_t read_meta_slot(int slot, video_meta_t *out)
{
    if (!g_nvs_open || !out || slot < 0 || slot > 1) return ESP_ERR_INVALID_ARG;
    const char *key = slot == 0 ? "m0" : "m1";
    size_t len = sizeof(*out);
    memset(out, 0, sizeof(*out));
    esp_err_t err = nvs_get_blob(g_nvs, key, out, &len);
    if (err != ESP_OK) return err;
    if (len != sizeof(*out)) return ESP_ERR_INVALID_SIZE;
    return meta_record_valid(out) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t commit_meta(const video_meta_t *record)
{
    if (!g_nvs_open || !record || !meta_record_valid(record)) return ESP_ERR_INVALID_ARG;
    int target_slot = (g_meta_slot == 0) ? 1 : 0;
    const char *key = target_slot == 0 ? "m0" : "m1";
    esp_err_t err = nvs_set_blob(g_nvs, key, record, sizeof(*record));
    if (err != ESP_OK) return err;
    err = nvs_commit(g_nvs);
    if (err != ESP_OK) return err;
    g_meta = *record;
    g_meta_slot = target_slot;
    return ESP_OK;
}

static video_meta_t next_meta(bool video_valid, uint8_t bank, uint32_t size, uint32_t video_crc)
{
    video_meta_t m;
    memset(&m, 0, sizeof(m));
    m.magic = VIDEO_META_MAGIC;
    m.version = VIDEO_META_VERSION;
    m.generation = (g_meta_slot >= 0) ? (g_meta.generation + 1u) : 1u;
    if (m.generation == 0) m.generation = 1u;
    m.active_bank = bank;
    m.video_valid = video_valid ? 1u : 0u;
    m.size = video_valid ? size : 0u;
    m.video_crc32 = video_valid ? video_crc : 0u;
    m.meta_crc32 = meta_crc(&m);
    return m;
}

esp_err_t prisma_video_ab_init(void)
{
    if (g_ready) return ESP_OK;

    g_bank[VIDEO_BANK_A] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY,
                                                     "video_a");
    g_bank[VIDEO_BANK_B] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY,
                                                     "video_b");
    if (!g_bank[VIDEO_BANK_A] || !g_bank[VIDEO_BANK_B]) {
        ESP_LOGE(TAG, "VIDEO partitions not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (g_bank[VIDEO_BANK_A]->size != PRISMA_VIDEO_MAX_BYTES ||
        g_bank[VIDEO_BANK_B]->size != PRISMA_VIDEO_MAX_BYTES) {
        ESP_LOGE(TAG, "Unexpected VIDEO bank size A=%u B=%u",
                 (unsigned)g_bank[VIDEO_BANK_A]->size,
                 (unsigned)g_bank[VIDEO_BANK_B]->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open("prisma_vid", NVS_READWRITE, &g_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    g_nvs_open = true;

    video_meta_t m0, m1;
    bool v0 = read_meta_slot(0, &m0) == ESP_OK;
    bool v1 = read_meta_slot(1, &m1) == ESP_OK;

    memset(&g_meta, 0, sizeof(g_meta));
    g_meta.active_bank = VIDEO_BANK_B; /* First successful upload targets A. */
    g_meta_slot = -1;

    if (v0 && v1) {
        if (generation_newer(m1.generation, m0.generation)) {
            g_meta = m1;
            g_meta_slot = 1;
        } else {
            g_meta = m0;
            g_meta_slot = 0;
        }
    } else if (v0) {
        g_meta = m0;
        g_meta_slot = 0;
    } else if (v1) {
        g_meta = m1;
        g_meta_slot = 1;
    }

    memset(&g_write, 0, sizeof(g_write));
    g_ready = true;

    if (g_meta_slot >= 0 && g_meta.video_valid) {
        ESP_LOGI(TAG, "READY active=%c size=%u crc=%08lx gen=%lu",
                 g_meta.active_bank == VIDEO_BANK_A ? 'A' : 'B',
                 (unsigned)g_meta.size,
                 (unsigned long)g_meta.video_crc32,
                 (unsigned long)g_meta.generation);
    } else {
        ESP_LOGI(TAG, "READY active=NONE gen=%lu",
                 (unsigned long)(g_meta_slot >= 0 ? g_meta.generation : 0u));
    }
    return ESP_OK;
}

esp_err_t prisma_video_ab_get_info(prisma_video_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->ready = g_ready;
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    out->valid = (g_meta_slot >= 0 && g_meta.video_valid != 0);
    out->active_bank = out->valid ? (g_meta.active_bank == VIDEO_BANK_A ? 'A' : 'B') : '-';
    out->generation = (g_meta_slot >= 0) ? g_meta.generation : 0u;
    out->size = out->valid ? g_meta.size : 0u;
    out->crc32 = out->valid ? g_meta.video_crc32 : 0u;
    return ESP_OK;
}

esp_err_t prisma_video_ab_begin(size_t expected_size)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (g_write.active) return ESP_ERR_INVALID_STATE;
    if (expected_size == 0 || expected_size > PRISMA_VIDEO_MAX_BYTES) return ESP_ERR_INVALID_SIZE;

    uint8_t target = (g_meta_slot >= 0) ? (uint8_t)(1u - g_meta.active_bank) : VIDEO_BANK_A;
    const esp_partition_t *p = g_bank[target];
    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase bank %c failed: %s", target == VIDEO_BANK_A ? 'A' : 'B', esp_err_to_name(err));
        return err;
    }

    memset(&g_write, 0, sizeof(g_write));
    g_write.active = true;
    g_write.bank = target;
    g_write.expected_size = expected_size;
    g_write.crc_state = 0xFFFFFFFFu;
    ESP_LOGI(TAG, "WRITE begin bank=%c size=%u", target == VIDEO_BANK_A ? 'A' : 'B', (unsigned)expected_size);
    return ESP_OK;
}

esp_err_t prisma_video_ab_write(size_t offset, const void *data, size_t len)
{
    if (!g_ready || !g_write.active) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (offset != g_write.written) return ESP_ERR_INVALID_ARG;
    if (offset > g_write.expected_size || len > g_write.expected_size - offset) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = esp_partition_write(g_bank[g_write.bank], offset, data, len);
    if (err != ESP_OK) return err;
    g_write.crc_state = crc32_update_state(g_write.crc_state, (const uint8_t *)data, len);
    g_write.written += len;
    return ESP_OK;
}

esp_err_t prisma_video_ab_finalize(void)
{
    if (!g_ready || !g_write.active) return ESP_ERR_INVALID_STATE;
    if (g_write.written != g_write.expected_size) return ESP_ERR_INVALID_SIZE;

    uint32_t video_crc = g_write.crc_state ^ 0xFFFFFFFFu;
    video_meta_t m = next_meta(true, g_write.bank, (uint32_t)g_write.expected_size, video_crc);
    esp_err_t err = commit_meta(&m);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Metadata commit failed; previous bank remains active: %s", esp_err_to_name(err));
        memset(&g_write, 0, sizeof(g_write));
        return err;
    }

    ESP_LOGI(TAG, "WRITE committed active=%c size=%u crc=%08lx gen=%lu",
             g_meta.active_bank == VIDEO_BANK_A ? 'A' : 'B',
             (unsigned)g_meta.size,
             (unsigned long)g_meta.video_crc32,
             (unsigned long)g_meta.generation);
    memset(&g_write, 0, sizeof(g_write));
    return ESP_OK;
}

void prisma_video_ab_abort(void)
{
    if (g_write.active) {
        ESP_LOGW(TAG, "WRITE aborted bank=%c after %u/%u bytes; active metadata unchanged",
                 g_write.bank == VIDEO_BANK_A ? 'A' : 'B',
                 (unsigned)g_write.written,
                 (unsigned)g_write.expected_size);
    }
    memset(&g_write, 0, sizeof(g_write));
}

esp_err_t prisma_video_ab_delete(void)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (g_write.active) return ESP_ERR_INVALID_STATE;
    uint8_t bank = (g_meta_slot >= 0) ? g_meta.active_bank : VIDEO_BANK_A;
    video_meta_t m = next_meta(false, bank, 0, 0);
    esp_err_t err = commit_meta(&m);
    if (err == ESP_OK) ESP_LOGI(TAG, "VIDEO invalidated atomically gen=%lu", (unsigned long)g_meta.generation);
    return err;
}

esp_err_t prisma_video_ab_read(size_t offset, void *dst, size_t len)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (!dst || len == 0) return ESP_ERR_INVALID_ARG;
    if (g_meta_slot < 0 || !g_meta.video_valid) return ESP_ERR_NOT_FOUND;
    if (offset > g_meta.size || len > g_meta.size - offset) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(g_bank[g_meta.active_bank], offset, dst, len);
}
