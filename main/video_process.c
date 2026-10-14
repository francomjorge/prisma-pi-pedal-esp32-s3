#include "video_process.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

#include "video_ab.h"

#define MAX_TEST_FRAMES 180
#define MAX_PIXEL_BYTES (512u * 1024u)
#define SCAN_CHUNK_BYTES 4096u

typedef struct {
    size_t offset;
    size_t length;
} frame_ref_t;

static SemaphoreHandle_t g_lock;
static size_t g_video_size;
static frame_ref_t g_frames[MAX_TEST_FRAMES];
static uint16_t g_frame_count;
static uint16_t *g_pixels;
static prisma_video_process_status_t g_status;

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return p;
}

static void set_error(const char *msg)
{
    if (!g_lock) return;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status.valid = false;
    strlcpy(g_status.error, msg ? msg : "video processing failed", sizeof(g_status.error));
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
}

esp_err_t prisma_video_process_init(void)
{
    if (g_lock) return ESP_OK;
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return ESP_ERR_NO_MEM;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

static bool baseline_size(const uint8_t *buf, size_t len, uint16_t *width, uint16_t *height)
{
    if (!buf || len < 10 || buf[0] != 0xFF || buf[1] != 0xD8) return false;
    size_t p = 2;
    while (p + 4 < len) {
        if (buf[p] != 0xFF) { p++; continue; }
        while (p < len && buf[p] == 0xFF) p++;
        if (p >= len) break;
        uint8_t marker = buf[p++];
        if (marker == 0xD8 || marker == 0xD9) continue;
        if (marker == 0xDA) break;
        if (p + 2 > len) break;
        uint16_t seglen = ((uint16_t)buf[p] << 8) | buf[p + 1];
        if (seglen < 2 || p + seglen > len) break;
        if (marker == 0xC0 && seglen >= 7) {
            *height = ((uint16_t)buf[p + 3] << 8) | buf[p + 4];
            *width = ((uint16_t)buf[p + 5] << 8) | buf[p + 6];
            return *width > 0 && *height > 0;
        }
        if (marker == 0xC2) return false;
        p += seglen;
    }
    return false;
}

static uint8_t choose_scale(uint16_t width, uint16_t height)
{
    const uint8_t divs[] = {1, 2, 4, 8};
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint32_t w = width / divs[i];
        uint32_t h = height / divs[i];
        size_t bytes = (size_t)w * h * sizeof(uint16_t);
        if (w && h && w <= 480 && h <= 480 && bytes <= MAX_PIXEL_BYTES) return divs[i];
    }
    return 0;
}

static esp_jpeg_image_scale_t scale_enum(uint8_t div)
{
    switch (div) {
        case 2: return JPEG_IMAGE_SCALE_1_2;
        case 4: return JPEG_IMAGE_SCALE_1_4;
        case 8: return JPEG_IMAGE_SCALE_1_8;
        default: return JPEG_IMAGE_SCALE_0;
    }
}

static uint16_t posterize_rgb565(uint16_t c)
{
    uint16_t r = (c >> 11) & 0x1F;
    uint16_t g = (c >> 5) & 0x3F;
    uint16_t b = c & 0x1F;
    uint16_t rl = (uint16_t)((r * 3u + 15u) / 31u);
    uint16_t gl = (uint16_t)((g * 3u + 31u) / 63u);
    uint16_t bl = (uint16_t)((b * 3u + 15u) / 31u);
    r = (uint16_t)((rl * 31u + 1u) / 3u);
    g = (uint16_t)(gl * 21u);
    b = (uint16_t)((bl * 31u + 1u) / 3u);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static esp_err_t index_video_from_flash(size_t total, frame_ref_t *refs, uint16_t *out_count)
{
    uint8_t *chunk = malloc(SCAN_CHUNK_BYTES);
    if (!chunk) return ESP_ERR_NO_MEM;

    bool in_frame = false;
    size_t soi = 0;
    uint8_t prev = 0;
    bool have_prev = false;
    uint16_t count = 0;

    for (size_t base = 0; base < total && count < MAX_TEST_FRAMES; base += SCAN_CHUNK_BYTES) {
        size_t len = total - base;
        if (len > SCAN_CHUNK_BYTES) len = SCAN_CHUNK_BYTES;
        esp_err_t err = prisma_video_ab_read(base, chunk, len);
        if (err != ESP_OK) {
            free(chunk);
            return err;
        }

        for (size_t i = 0; i < len && count < MAX_TEST_FRAMES; ++i) {
            uint8_t cur = chunk[i];
            size_t pos = base + i;
            if (have_prev) {
                if (!in_frame && prev == 0xFF && cur == 0xD8) {
                    soi = pos - 1;
                    in_frame = true;
                } else if (in_frame && prev == 0xFF && cur == 0xD9) {
                    refs[count].offset = soi;
                    refs[count].length = (pos + 1) - soi;
                    count++;
                    in_frame = false;
                }
            }
            prev = cur;
            have_prev = true;
        }
    }

    free(chunk);
    *out_count = count;
    return ESP_OK;
}

static esp_err_t read_frame(const frame_ref_t *ref, uint8_t **out)
{
    if (!ref || !out || ref->length < 4 || ref->offset + ref->length > g_video_size) return ESP_ERR_INVALID_ARG;
    uint8_t *jpeg = psram_alloc(ref->length);
    if (!jpeg) return ESP_ERR_NO_MEM;
    esp_err_t err = prisma_video_ab_read(ref->offset, jpeg, ref->length);
    if (err != ESP_OK) {
        free(jpeg);
        return err;
    }
    *out = jpeg;
    return ESP_OK;
}

esp_err_t prisma_video_process_prepare(void)
{
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    prisma_video_info_t info = {0};
    if (prisma_video_ab_get_info(&info) != ESP_OK || !info.valid || info.size == 0) {
        set_error("no active VIDEO in A/B storage");
        return ESP_ERR_NOT_FOUND;
    }

    frame_ref_t refs[MAX_TEST_FRAMES];
    uint16_t count = 0;
    esp_err_t err = index_video_from_flash(info.size, refs, &count);
    if (err != ESP_OK) {
        set_error("could not index VIDEO directly from flash");
        return err;
    }
    if (count == 0) {
        set_error("VIDEO test payload has no complete JPEG frames");
        return ESP_ERR_NOT_SUPPORTED;
    }

    g_video_size = info.size;
    uint8_t *first = NULL;
    err = read_frame(&refs[0], &first);
    if (err != ESP_OK) {
        set_error("could not read first VIDEO frame from flash");
        return err;
    }

    uint16_t w = 0, h = 0;
    bool baseline = baseline_size(first, refs[0].length, &w, &h);
    free(first);
    if (!baseline) {
        set_error("VIDEO frames must be baseline JPEG");
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    memcpy(g_frames, refs, count * sizeof(frame_ref_t));
    g_frame_count = count;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.prepared = true;
    g_status.source_generation = info.generation;
    g_status.source_bytes = info.size;
    g_status.frame_count = count;
    g_status.width = w;
    g_status.height = h;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t prisma_video_process_render_frame(uint16_t index)
{
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool prepared = g_status.prepared && index < g_frame_count;
    frame_ref_t ref = prepared ? g_frames[index] : (frame_ref_t){0};
    xSemaphoreGive(g_lock);
    if (!prepared) return ESP_ERR_INVALID_ARG;

    int64_t started = esp_timer_get_time();
    uint8_t *jpeg = NULL;
    esp_err_t err = read_frame(&ref, &jpeg);
    if (err != ESP_OK) {
        set_error("could not read VIDEO frame from flash");
        return err;
    }

    uint16_t src_w = 0, src_h = 0;
    if (!baseline_size(jpeg, ref.length, &src_w, &src_h)) {
        free(jpeg);
        set_error("frame is not baseline JPEG");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t div = choose_scale(src_w, src_h);
    if (!div) {
        free(jpeg);
        set_error("frame too large for processing buffer");
        return ESP_ERR_NO_MEM;
    }
    uint16_t out_w = src_w / div;
    uint16_t out_h = src_h / div;
    size_t out_bytes = (size_t)out_w * out_h * sizeof(uint16_t);
    uint16_t *pixels = psram_alloc(out_bytes);
    if (!pixels) {
        free(jpeg);
        set_error("not enough PSRAM for decoded frame");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg,
        .indata_size = ref.length,
        .outbuf = (uint8_t *)pixels,
        .outbuf_size = out_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale_enum(div),
        .flags = { .swap_color_bytes = 0 },
    };
    esp_jpeg_image_output_t decoded = {0};
    err = esp_jpeg_decode(&cfg, &decoded);
    free(jpeg);
    if (err != ESP_OK) {
        free(pixels);
        set_error("ESP JPEG decoder failed on VIDEO frame");
        return err;
    }

    size_t n = (size_t)decoded.width * decoded.height;
    for (size_t i = 0; i < n; ++i) pixels[i] = posterize_rgb565(pixels[i]);

    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    uint16_t *old = g_pixels;
    g_pixels = pixels;
    g_status.valid = true;
    g_status.error[0] = '\0';
    g_status.last_frame = index;
    g_status.width = decoded.width;
    g_status.height = decoded.height;
    g_status.last_frame_ms = elapsed;
    g_status.pixel_bytes = (size_t)decoded.width * decoded.height * sizeof(uint16_t);
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
    free(old);
    return ESP_OK;
}

esp_err_t prisma_video_process_get_status(prisma_video_process_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    *out = g_status;
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t prisma_video_process_copy_scaled_rgb565(uint16_t *dst,
                                                  uint16_t dst_width,
                                                  uint16_t dst_height,
                                                  bool byte_swap,
                                                  uint16_t *src_width,
                                                  uint16_t *src_height)
{
    if (!dst || !dst_width || !dst_height) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_status.valid || !g_pixels || !g_status.width || !g_status.height) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }

    const uint16_t sw = g_status.width;
    const uint16_t sh = g_status.height;
    if (src_width) *src_width = sw;
    if (src_height) *src_height = sh;

    for (size_t i = 0; i < (size_t)dst_width * dst_height; ++i) dst[i] = 0;

    uint32_t fw = dst_width;
    uint32_t fh = ((uint32_t)sh * dst_width) / sw;
    if (fh > dst_height) {
        fh = dst_height;
        fw = ((uint32_t)sw * dst_height) / sh;
    }
    if (!fw) fw = 1;
    if (!fh) fh = 1;
    uint16_t x0 = (uint16_t)((dst_width - fw) / 2u);
    uint16_t y0 = (uint16_t)((dst_height - fh) / 2u);

    for (uint16_t y = 0; y < fh; ++y) {
        uint16_t sy = (uint16_t)(((uint32_t)y * sh) / fh);
        if (sy >= sh) sy = sh - 1;
        uint16_t *d = dst + (size_t)(y0 + y) * dst_width + x0;
        for (uint16_t x = 0; x < fw; ++x) {
            uint16_t sx = (uint16_t)(((uint32_t)x * sw) / fw);
            if (sx >= sw) sx = sw - 1;
            uint16_t c = g_pixels[(size_t)sy * sw + sx];
            if (byte_swap) c = (uint16_t)((c >> 8) | (c << 8));
            d[x] = c;
        }
    }

    xSemaphoreGive(g_lock);
    return ESP_OK;
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

esp_err_t prisma_video_process_send_bmp(httpd_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_status.valid || !g_pixels) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const uint16_t w = g_status.width;
    const uint16_t h = g_status.height;
    const uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
    const uint32_t image_bytes = row_bytes * h;
    const uint32_t file_bytes = 54u + image_bytes;

    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    le32(&header[2], file_bytes);
    le32(&header[10], 54);
    le32(&header[14], 40);
    le32(&header[18], w);
    le32(&header[22], h);
    le16(&header[26], 1);
    le16(&header[28], 24);
    le32(&header[34], image_bytes);

    uint8_t *row = malloc(row_bytes);
    if (!row) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    for (int y = h - 1; err == ESP_OK && y >= 0; --y) {
        memset(row, 0, row_bytes);
        for (uint16_t x = 0; x < w; ++x) {
            uint16_t c = g_pixels[(size_t)y * w + x];
            row[x * 3u + 0] = (uint8_t)((c & 0x1F) * 255u / 31u);
            row[x * 3u + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255u / 63u);
            row[x * 3u + 2] = (uint8_t)(((c >> 11) & 0x1F) * 255u / 31u);
        }
        err = httpd_resp_send_chunk(req, (const char *)row, row_bytes);
    }
    free(row);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(g_lock);
    return err;
}
