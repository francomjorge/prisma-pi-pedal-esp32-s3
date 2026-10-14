#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lowzag.h"

#include "image_ab.h"
#include "local_process.h"
#include "poster_config.h"
#include "processed_store.h"

#define JPEG_COMMON_PSRAM_RESERVE (1536u * 1024u)

static const char *TAG = "PRISMA_JPEG_COMMON";
static bool s_progressive_active = false;
static prisma_process_status_t s_status;

esp_err_t __real_prisma_process_run_poster(void);
esp_err_t __real_prisma_process_run_lines(void);
esp_err_t __real_prisma_process_run_rectangles(void);
esp_err_t __real_prisma_process_run_circles(void);
esp_err_t __real_prisma_process_run_triangles(void);
esp_err_t __real_prisma_process_get_status(prisma_process_status_t *out);
esp_err_t __real_prisma_process_send_bmp(httpd_req_t *req);

typedef struct {
    const uint8_t *p;
    size_t left;
} progressive_src_t;

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    bool overflow;
} progressive_sink_t;

static int progressive_read(void *ctx, uint8_t *buf, size_t len)
{
    progressive_src_t *s = (progressive_src_t *)ctx;
    if (len > s->left) len = s->left;
    memcpy(buf, s->p, len);
    s->p += len;
    s->left -= len;
    return (int)len;
}

static inline uint8_t level5(uint16_t v)
{
    if (v <= 5) return 0;
    if (v <= 15) return 1;
    if (v <= 25) return 2;
    return 3;
}

static inline uint8_t level6(uint16_t v)
{
    if (v <= 10) return 0;
    if (v <= 31) return 1;
    if (v <= 52) return 2;
    return 3;
}

static inline uint8_t poster_index_rgb888(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rl = level5((uint16_t)(r >> 3));
    uint8_t gl = level6((uint16_t)(g >> 2));
    uint8_t bl = level5((uint16_t)(b >> 3));
    return (uint8_t)((rl << 4) | (gl << 2) | bl);
}

static void progressive_row(void *ctx, int y, int w, const uint8_t *rgb)
{
    progressive_sink_t *s = (progressive_sink_t *)ctx;
    if (!s || !s->pixels || y < 0 || y >= s->height || w != s->width) {
        if (s) s->overflow = true;
        return;
    }
    uint8_t *dst = s->pixels + (size_t)y * (size_t)s->width;
    for (int x = 0; x < w; ++x) {
        dst[x] = poster_index_rgb888(rgb[x * 3], rgb[x * 3 + 1], rgb[x * 3 + 2]);
    }
}

static void *progressive_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void progressive_free(void *ctx, void *p)
{
    (void)ctx;
    heap_caps_free(p);
}

static int progressive_cancel(void *ctx)
{
    unsigned *polls = (unsigned *)ctx;
    if (polls) {
        (*polls)++;
        if (((*polls) & 31u) == 0u) taskYIELD();
    }
    return 0;
}

static bool jpeg_progressive_size(const uint8_t *buf, size_t len, uint16_t *width, uint16_t *height)
{
    if (!buf || !width || !height || len < 10 || buf[0] != 0xFF || buf[1] != 0xD8) return false;
    size_t p = 2;
    while (p + 4 < len) {
        if (buf[p] != 0xFF) { p++; continue; }
        while (p < len && buf[p] == 0xFF) p++;
        if (p >= len) break;
        uint8_t marker = buf[p++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xDA) break;
        if (p + 2 > len) break;
        uint16_t seglen = ((uint16_t)buf[p] << 8) | buf[p + 1];
        if (seglen < 2 || p + seglen > len) break;
        if (marker == 0xC2 && seglen >= 7) {
            *height = ((uint16_t)buf[p + 3] << 8) | buf[p + 4];
            *width = ((uint16_t)buf[p + 5] << 8) | buf[p + 6];
            return *width > 0 && *height > 0;
        }
        if (marker == 0xC0 || marker == 0xC1) return false;
        p += seglen;
    }
    return false;
}

static void crop_for_target(uint16_t sw, uint16_t sh, uint16_t tw, uint16_t th,
                            uint16_t *x0, uint16_t *y0, uint16_t *cw, uint16_t *ch)
{
    *x0 = 0; *y0 = 0; *cw = sw; *ch = sh;
    uint32_t lhs = (uint32_t)sw * th;
    uint32_t rhs = (uint32_t)sh * tw;
    if (lhs > rhs) {
        uint16_t w = (uint16_t)(((uint32_t)sh * tw) / th);
        if (!w) w = 1;
        *cw = w;
        *x0 = (uint16_t)((sw - w) / 2u);
    } else if (lhs < rhs) {
        uint16_t h = (uint16_t)(((uint32_t)sw * th) / tw);
        if (!h) h = 1;
        *ch = h;
        *y0 = (uint16_t)((sh - h) / 2u);
    }
}

static void crop_scale_index(const uint8_t *src, uint16_t sw, uint16_t sh,
                             uint8_t *dst, uint16_t tw, uint16_t th)
{
    uint16_t x0, y0, cw, ch;
    crop_for_target(sw, sh, tw, th, &x0, &y0, &cw, &ch);
    for (uint16_t y = 0; y < th; ++y) {
        uint16_t sy = (uint16_t)(y0 + ((uint32_t)y * ch) / th);
        if (sy >= sh) sy = sh - 1;
        for (uint16_t x = 0; x < tw; ++x) {
            uint16_t sx = (uint16_t)(x0 + ((uint32_t)x * cw) / tw);
            if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * tw + x] = src[(size_t)sy * sw + sx];
        }
    }
}

static void scale_index(const uint8_t *src, uint16_t sw, uint16_t sh,
                        uint8_t *dst, uint16_t tw, uint16_t th)
{
    for (uint16_t y = 0; y < th; ++y) {
        uint16_t sy = (uint16_t)(((uint32_t)y * sh) / th);
        if (sy >= sh) sy = sh - 1;
        for (uint16_t x = 0; x < tw; ++x) {
            uint16_t sx = (uint16_t)(((uint32_t)x * sw) / tw);
            if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * tw + x] = src[(size_t)sy * sw + sx];
        }
    }
}

static esp_err_t store_index(prisma_processed_slot_t slot, const uint8_t *pixels,
                             uint16_t w, uint16_t h, uint32_t generation, uint32_t hash)
{
    size_t n = (size_t)w * h;
    esp_err_t err = prisma_processed_begin(slot, PRISMA_PROCESSED_FMT_INDEX8,
                                           generation, hash, w, h);
    if (err != ESP_OK) return err;
    err = prisma_processed_write(slot, 0, pixels, n);
    if (err == ESP_OK) err = prisma_processed_finalize(slot, 1, 1, 0);
    if (err != ESP_OK) prisma_processed_abort(slot);
    return err;
}

static void status_begin(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.ready = true;
    s_status.busy = true;
    strlcpy(s_status.last_test, "poster", sizeof(s_status.last_test));
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_progressive_active = true;
}

static esp_err_t status_fail(esp_err_t err, const char *message)
{
    s_status.busy = false;
    s_status.valid = false;
    strlcpy(s_status.error, message ? message : "progressive JPEG failed", sizeof(s_status.error));
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "%s", s_status.error);
    return err;
}

static void status_commit(uint16_t sw, uint16_t sh, uint16_t w, uint16_t h,
                          uint32_t elapsed, uint32_t generation, uint8_t div, size_t payload)
{
    s_status.busy = false;
    s_status.valid = true;
    s_status.error[0] = '\0';
    s_status.source_width = sw;
    s_status.source_height = sh;
    s_status.width = w;
    s_status.height = h;
    s_status.elapsed_ms = elapsed;
    s_status.source_generation = generation;
    s_status.pixel_bytes = payload;
    s_status.jpeg_scale_div = div;
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static esp_err_t decode_progressive(const uint8_t *jpeg, size_t jpeg_size,
                                    uint16_t target_w, uint16_t target_h,
                                    uint8_t **out_pixels, uint16_t *out_w, uint16_t *out_h,
                                    uint8_t *out_div)
{
    if (!jpeg || !jpeg_size || !out_pixels || !out_w || !out_h || !out_div) return ESP_ERR_INVALID_ARG;
    *out_pixels = NULL;

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= JPEG_COMMON_PSRAM_RESERVE) return ESP_ERR_NO_MEM;
    size_t budget = free_psram - JPEG_COMMON_PSRAM_RESERVE;

    lowzag_cfg_t cfg = LOWZAG_CFG_INIT;
    cfg.target_w = target_w;
    cfg.target_h = target_h;
    cfg.mem_limit = budget;

    lowzag_estimate_t est = LOWZAG_ESTIMATE_INIT;
    lowzag_err_t le = LOWZAG_OK;
    for (int attempt = 0; attempt < 4; ++attempt) {
        est = (lowzag_estimate_t)LOWZAG_ESTIMATE_INIT;
        le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
        if (le != LOWZAG_OK) break;
        if (est.out_w <= 0 || est.out_h <= 0 || est.out_w > UINT16_MAX || est.out_h > UINT16_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t pixels = (size_t)est.out_w * (size_t)est.out_h;
        if (pixels >= budget) return ESP_ERR_NO_MEM;
        size_t decoder_limit = budget - pixels;
        if (est.alloc_bytes <= decoder_limit && cfg.mem_limit <= decoder_limit) break;
        cfg.mem_limit = decoder_limit;
    }
    if (le != LOWZAG_OK) {
        ESP_LOGE(TAG, "lowzag estimate failed: %s", lowzag_strerror(le));
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }

    est = (lowzag_estimate_t)LOWZAG_ESTIMATE_INIT;
    le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
    if (le != LOWZAG_OK) {
        ESP_LOGE(TAG, "lowzag final estimate failed: %s", lowzag_strerror(le));
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }

    size_t pixel_bytes = (size_t)est.out_w * (size_t)est.out_h;
    uint8_t *pixels = heap_caps_malloc(pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return ESP_ERR_NO_MEM;

    void *state = heap_caps_aligned_calloc(lowzag_state_align(), 1, lowzag_state_size(),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!state) {
        heap_caps_free(pixels);
        return ESP_ERR_NO_MEM;
    }

    progressive_src_t src = {.p = jpeg, .left = jpeg_size};
    progressive_sink_t sink = {
        .pixels = pixels,
        .width = est.out_w,
        .height = est.out_h,
        .overflow = false,
    };
    unsigned polls = 0;
    cfg.read = progressive_read;
    cfg.read_ctx = &src;
    cfg.row = progressive_row;
    cfg.row_ctx = &sink;
    cfg.alloc = progressive_alloc;
    cfg.free = progressive_free;
    cfg.cancel = progressive_cancel;
    cfg.cancel_ctx = &polls;
    cfg.state = state;

    lowzag_info_t info = LOWZAG_INFO_INIT;
    le = lowzag_decode(&cfg, &info);
    heap_caps_free(state);
    if (le != LOWZAG_OK || sink.overflow || info.out_w != est.out_w || info.out_h != est.out_h) {
        ESP_LOGE(TAG, "lowzag decode failed: %s overflow=%s", lowzag_strerror(le), sink.overflow ? "yes" : "no");
        heap_caps_free(pixels);
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }

    *out_pixels = pixels;
    *out_w = (uint16_t)info.out_w;
    *out_h = (uint16_t)info.out_h;
    *out_div = (uint8_t)info.scale;
    ESP_LOGI(TAG, "progressive decode %dx%d -> %dx%d scale=1/%d peak=%uKB",
             info.src_w, info.src_h, info.out_w, info.out_h, info.scale,
             (unsigned)((info.peak_alloc + 1023u) / 1024u));
    return ESP_OK;
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static esp_err_t send_index_bmp(httpd_req_t *req, prisma_processed_slot_t slot)
{
    prisma_processed_info_t in = {0};
    esp_err_t err = prisma_processed_get_info(slot, &in);
    if (err != ESP_OK || !in.valid || in.format != PRISMA_PROCESSED_FMT_INDEX8) return ESP_ERR_NOT_FOUND;
    uint16_t w = in.width, h = in.height;
    uint32_t po = 54u + 64u * 4u, rowb = ((uint32_t)w + 3u) & ~3u, img = rowb * h, file = po + img;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M'; le32(&hdr[2], file); le32(&hdr[10], po); le32(&hdr[14], 40);
    le32(&hdr[18], w); le32(&hdr[22], h); le16(&hdr[26], 1); le16(&hdr[28], 8); le32(&hdr[34], img); le32(&hdr[46], 64); le32(&hdr[50], 64);
    static const uint8_t rb[4] = {0,82,173,255}, gg[4] = {0,85,170,255};
    uint8_t pal[256];
    for (int i = 0; i < 64; ++i) {
        uint8_t r = (i >> 4) & 3, g = (i >> 2) & 3, b = i & 3;
        pal[i * 4] = rb[b]; pal[i * 4 + 1] = gg[g]; pal[i * 4 + 2] = rb[r]; pal[i * 4 + 3] = 0;
    }
    uint8_t *row = malloc(rowb);
    if (!row) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)pal, sizeof(pal));
    for (int y = h - 1; err == ESP_OK && y >= 0; --y) {
        memset(row, 0, rowb);
        err = prisma_processed_read(slot, (size_t)y * w, row, w);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)row, rowb);
    }
    free(row);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

esp_err_t __wrap_prisma_process_run_poster(void)
{
    prisma_image_info_t image = {0};
    if (prisma_image_ab_get_info(&image) != ESP_OK || !image.valid || !image.size) {
        s_progressive_active = false;
        return __real_prisma_process_run_poster();
    }

    uint8_t *jpeg = heap_caps_malloc(image.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        s_progressive_active = false;
        return __real_prisma_process_run_poster();
    }
    esp_err_t err = prisma_image_ab_read(0, jpeg, image.size);
    if (err != ESP_OK) {
        heap_caps_free(jpeg);
        s_progressive_active = false;
        return __real_prisma_process_run_poster();
    }

    uint16_t sw = 0, sh = 0;
    if (!jpeg_progressive_size(jpeg, image.size, &sw, &sh)) {
        heap_caps_free(jpeg);
        s_progressive_active = false;
        return __real_prisma_process_run_poster();
    }

    status_begin();
    int64_t started = esp_timer_get_time();

    prisma_poster_config_t pcfg;
    (void)prisma_poster_config_load(&pcfg);
    uint32_t cfg_hash = prisma_poster_config_hash(&pcfg, false);

    prisma_processed_info_t cached = {0}, preview = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &cached);
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_PREVIEW, &preview);
    if (cached.valid && preview.valid && cached.source_generation == image.generation &&
        preview.source_generation == image.generation && cached.config_hash == cfg_hash &&
        preview.config_hash == cfg_hash) {
        heap_caps_free(jpeg);
        status_commit(sw, sh, cached.width, cached.height, 0, image.generation, 1, cached.payload_size);
        return ESP_OK;
    }

    uint16_t tw = 0, th = 0;
    if (!prisma_poster_resolve_output(pcfg.image_preset, pcfg.image_aspect, sw, sh, &tw, &th)) {
        heap_caps_free(jpeg);
        return status_fail(ESP_ERR_INVALID_ARG, "invalid IMAGE output configuration");
    }

    uint8_t *decoded = NULL;
    uint16_t dw = 0, dh = 0;
    uint8_t div = 0;
    err = decode_progressive(jpeg, image.size, tw, th, &decoded, &dw, &dh, &div);
    heap_caps_free(jpeg);
    if (err != ESP_OK) {
        return status_fail(err, err == ESP_ERR_NO_MEM ?
                           "progressive JPEG exceeds safe PSRAM workspace" :
                           "progressive JPEG decoder failed");
    }

    uint8_t *index = heap_caps_malloc((size_t)tw * th, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!index) {
        heap_caps_free(decoded);
        return status_fail(ESP_ERR_NO_MEM, "not enough PSRAM for IMAGE output");
    }
    crop_scale_index(decoded, dw, dh, index, tw, th);
    heap_caps_free(decoded);

    err = store_index(PRISMA_PROCESSED_IMAGE_OUT, index, tw, th, image.generation, cfg_hash);
    if (err != ESP_OK) {
        heap_caps_free(index);
        return status_fail(err, "could not commit IMAGE OUT to flash");
    }

    uint16_t pw = 0, ph = 0;
    if (!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166, pcfg.image_aspect, sw, sh, &pw, &ph)) {
        heap_caps_free(index);
        return status_fail(ESP_FAIL, "could not resolve 166P preview");
    }
    uint8_t *small = heap_caps_malloc((size_t)pw * ph, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!small) {
        heap_caps_free(index);
        return status_fail(ESP_ERR_NO_MEM, "not enough PSRAM for 166P preview");
    }
    scale_index(index, tw, th, small, pw, ph);
    heap_caps_free(index);
    err = store_index(PRISMA_PROCESSED_IMAGE_PREVIEW, small, pw, ph, image.generation, cfg_hash);
    heap_caps_free(small);
    if (err != ESP_OK) return status_fail(err, "could not commit IMAGE PREVIEW 166 to flash");

    prisma_processed_info_t out = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &out);
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    status_commit(sw, sh, tw, th, elapsed, image.generation, div, out.payload_size);
    ESP_LOGI(TAG, "POSTER progressive source=%ux%u decoded=%ux%u output=%ux%u preview=%ux%u div=%u %lums",
             sw, sh, dw, dh, tw, th, pw, ph, div, (unsigned long)elapsed);
    return ESP_OK;
}

esp_err_t __wrap_prisma_process_get_status(prisma_process_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_progressive_active) return __real_prisma_process_get_status(out);
    *out = s_status;
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

esp_err_t __wrap_prisma_process_send_bmp(httpd_req_t *req)
{
    if (!s_progressive_active) return __real_prisma_process_send_bmp(req);
    if (!s_status.valid) return ESP_ERR_NOT_FOUND;
    return send_index_bmp(req, PRISMA_PROCESSED_IMAGE_OUT);
}

esp_err_t __wrap_prisma_process_run_lines(void)
{
    s_progressive_active = false;
    return __real_prisma_process_run_lines();
}

esp_err_t __wrap_prisma_process_run_rectangles(void)
{
    s_progressive_active = false;
    return __real_prisma_process_run_rectangles();
}

esp_err_t __wrap_prisma_process_run_circles(void)
{
    s_progressive_active = false;
    return __real_prisma_process_run_circles();
}

esp_err_t __wrap_prisma_process_run_triangles(void)
{
    s_progressive_active = false;
    return __real_prisma_process_run_triangles();
}
