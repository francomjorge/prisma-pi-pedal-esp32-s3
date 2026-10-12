#include "local_process.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

#include "image_ab.h"

#define PROCESS_MAX_PIXELS_BYTES (512u * 1024u)
#define LINE_W 240
#define LINE_H 240

static const char *TAG = "PRISMA_PROCESS";

static SemaphoreHandle_t g_lock;
static uint16_t *g_pixels;
static prisma_process_status_t g_status;

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return p;
}

static void status_error(const char *test, const char *message)
{
    if (!g_lock) return;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status.busy = false;
    if (test) strlcpy(g_status.last_test, test, sizeof(g_status.last_test));
    strlcpy(g_status.error, message ? message : "processing failed", sizeof(g_status.error));
    xSemaphoreGive(g_lock);
}

static bool begin_test(const char *name)
{
    if (!g_lock && prisma_process_init() != ESP_OK) return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_status.busy) {
        xSemaphoreGive(g_lock);
        return false;
    }
    g_status.busy = true;
    g_status.error[0] = '\0';
    strlcpy(g_status.last_test, name, sizeof(g_status.last_test));
    xSemaphoreGive(g_lock);
    return true;
}

static void commit_result(const char *name, uint16_t *pixels, uint16_t width, uint16_t height,
                          uint32_t elapsed_ms, uint32_t source_generation, uint8_t scale_div)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    uint16_t *old = g_pixels;
    g_pixels = pixels;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.valid = true;
    strlcpy(g_status.last_test, name, sizeof(g_status.last_test));
    g_status.width = width;
    g_status.height = height;
    g_status.elapsed_ms = elapsed_ms;
    g_status.source_generation = source_generation;
    g_status.pixel_bytes = (size_t)width * height * sizeof(uint16_t);
    g_status.jpeg_scale_div = scale_div;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
    free(old);
}

esp_err_t prisma_process_init(void)
{
    if (g_lock) return ESP_OK;
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return ESP_ERR_NO_MEM;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

static uint8_t choose_scale_div(uint16_t width, uint16_t height)
{
    const uint8_t divs[] = {1, 2, 4, 8};
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint32_t w = width / divs[i];
        uint32_t h = height / divs[i];
        size_t bytes = (size_t)w * h * sizeof(uint16_t);
        if (w > 0 && h > 0 && w <= 480 && h <= 480 && bytes <= PROCESS_MAX_PIXELS_BYTES) {
            return divs[i];
        }
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

static bool jpeg_baseline_size(const uint8_t *buf, size_t len, uint16_t *width, uint16_t *height)
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
        if (marker == 0xC2) return false; /* progressive JPEG: intentionally rejected */
        p += seglen;
    }
    return false;
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

esp_err_t prisma_process_run_poster(void)
{
    if (!begin_test("poster")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();

    prisma_image_info_t info = {0};
    if (prisma_image_ab_get_info(&info) != ESP_OK || !info.valid || info.size == 0) {
        status_error("poster", "no active IMAGE in A/B storage");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *jpeg = psram_alloc(info.size);
    if (!jpeg) {
        status_error("poster", "not enough memory for JPEG source");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = prisma_image_ab_read(0, jpeg, info.size);
    if (err != ESP_OK) {
        free(jpeg);
        status_error("poster", "could not read active IMAGE from flash");
        return err;
    }

    uint16_t src_w = 0, src_h = 0;
    if (!jpeg_baseline_size(jpeg, info.size, &src_w, &src_h)) {
        free(jpeg);
        status_error("poster", "JPEG must be baseline (non-progressive)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t div = choose_scale_div(src_w, src_h);
    if (!div) {
        free(jpeg);
        status_error("poster", "decoded IMAGE is too large for smoke-test buffer");
        return ESP_ERR_NO_MEM;
    }

    uint16_t out_w = src_w / div;
    uint16_t out_h = src_h / div;
    size_t out_bytes = (size_t)out_w * out_h * sizeof(uint16_t);
    uint16_t *pixels = psram_alloc(out_bytes);
    if (!pixels) {
        free(jpeg);
        status_error("poster", "not enough RAM/PSRAM for decoded IMAGE");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg,
        .indata_size = info.size,
        .outbuf = (uint8_t *)pixels,
        .outbuf_size = out_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale_enum(div),
        .flags = {
            .swap_color_bytes = 0,
        },
    };
    esp_jpeg_image_output_t decoded = {0};
    err = esp_jpeg_decode(&cfg, &decoded);
    free(jpeg);
    if (err != ESP_OK) {
        free(pixels);
        status_error("poster", "ESP JPEG decoder failed");
        return err;
    }

    size_t count = (size_t)decoded.width * decoded.height;
    for (size_t i = 0; i < count; ++i) pixels[i] = posterize_rgb565(pixels[i]);

    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    commit_result("poster", pixels, decoded.width, decoded.height, elapsed, info.generation, div);
    ESP_LOGI(TAG, "POSTER local source_gen=%lu %ux%u div=%u %lums",
             (unsigned long)info.generation, decoded.width, decoded.height, div, (unsigned long)elapsed);
    return ESP_OK;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static void draw_line(uint16_t *px, int w, int h, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        if ((unsigned)x0 < (unsigned)w && (unsigned)y0 < (unsigned)h) px[y0 * w + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

esp_err_t prisma_process_run_lines(void)
{
    if (!begin_test("lines")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();
    size_t bytes = LINE_W * LINE_H * sizeof(uint16_t);
    uint16_t *pixels = psram_alloc(bytes);
    if (!pixels) {
        status_error("lines", "not enough RAM/PSRAM for line buffer");
        return ESP_ERR_NO_MEM;
    }

    uint16_t bg = rgb565(4, 7, 12);
    uint16_t c1 = rgb565(40, 215, 215);
    uint16_t c2 = rgb565(240, 245, 250);
    for (size_t i = 0; i < LINE_W * LINE_H; ++i) pixels[i] = bg;

    for (int i = 0; i < 18; ++i) {
        int y = 8 + i * 13;
        draw_line(pixels, LINE_W, LINE_H, 8, y, LINE_W - 9, LINE_H - 1 - y, (i & 1) ? c1 : c2);
    }
    for (int i = 0; i < 8; ++i) {
        int x = 18 + i * 29;
        draw_line(pixels, LINE_W, LINE_H, x, 8, LINE_W - 1 - x, LINE_H - 9, c1);
    }

    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    commit_result("lines", pixels, LINE_W, LINE_H, elapsed, 0, 1);
    ESP_LOGI(TAG, "LINES local %dx%d %lums", LINE_W, LINE_H, (unsigned long)elapsed);
    return ESP_OK;
}

esp_err_t prisma_process_get_status(prisma_process_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    *out = g_status;
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
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

esp_err_t prisma_process_send_bmp(httpd_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

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
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=prisma-process.bmp");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    for (int y = h - 1; err == ESP_OK && y >= 0; --y) {
        memset(row, 0, row_bytes);
        for (uint16_t x = 0; x < w; ++x) {
            uint16_t c = g_pixels[(size_t)y * w + x];
            uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255u / 31u);
            uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255u / 63u);
            uint8_t b = (uint8_t)((c & 0x1F) * 255u / 31u);
            row[x * 3u + 0] = b;
            row[x * 3u + 1] = g;
            row[x * 3u + 2] = r;
        }
        err = httpd_resp_send_chunk(req, (const char *)row, row_bytes);
    }
    free(row);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(g_lock);
    return err;
}
