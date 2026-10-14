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
#include "jpeg_decoder.h"
#include "lowzag.h"

#include "image_ab.h"
#include "local_process.h"
#include "poster_config.h"
#include "processed_store.h"

#define JPEG_WORKSPACE_RESERVE (1536u * 1024u)

static const char *TAG = "PRISMA_JPEG_ADAPT";
static bool s_override_active;
static prisma_process_status_t s_status;

typedef enum {
    JPEG_KIND_UNKNOWN = 0,
    JPEG_KIND_BASELINE,
    JPEG_KIND_PROGRESSIVE,
} jpeg_kind_t;

typedef struct {
    const uint8_t *p;
    size_t left;
} lowzag_src_t;

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    bool overflow;
} lowzag_sink_t;

esp_err_t __real_prisma_process_run_poster(void);
esp_err_t __real_prisma_process_run_lines(void);
esp_err_t __real_prisma_process_run_rectangles(void);
esp_err_t __real_prisma_process_run_circles(void);
esp_err_t __real_prisma_process_run_triangles(void);
esp_err_t __real_prisma_process_get_status(prisma_process_status_t *out);
esp_err_t __real_prisma_process_send_bmp(httpd_req_t *req);

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return p;
}

static jpeg_kind_t jpeg_probe(const uint8_t *buf, size_t len, uint16_t *width, uint16_t *height)
{
    if (!buf || !width || !height || len < 10 || buf[0] != 0xFF || buf[1] != 0xD8)
        return JPEG_KIND_UNKNOWN;
    size_t p = 2;
    while (p + 4 < len) {
        if (buf[p] != 0xFF) { p++; continue; }
        while (p < len && buf[p] == 0xFF) p++;
        if (p >= len) break;
        uint8_t marker = buf[p++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xDA) break;
        if (p + 2 > len) break;
        uint16_t seglen = ((uint16_t)buf[p] << 8) | buf[p + 1];
        if (seglen < 2 || p + seglen > len) break;
        if ((marker == 0xC0 || marker == 0xC2) && seglen >= 7) {
            *height = ((uint16_t)buf[p + 3] << 8) | buf[p + 4];
            *width = ((uint16_t)buf[p + 5] << 8) | buf[p + 6];
            if (!*width || !*height) return JPEG_KIND_UNKNOWN;
            return marker == 0xC2 ? JPEG_KIND_PROGRESSIVE : JPEG_KIND_BASELINE;
        }
        p += seglen;
    }
    return JPEG_KIND_UNKNOWN;
}

static bool resolve_fitted(prisma_poster_preset_t preset,
                           prisma_poster_aspect_t aspect,
                           uint16_t sw, uint16_t sh,
                           size_t capacity,
                           uint16_t *tw, uint16_t *th,
                           bool *fell_back)
{
    if (fell_back) *fell_back = false;
    if (!prisma_poster_resolve_output(preset, aspect, sw, sh, tw, th)) return false;
    if ((size_t)(*tw) * (*th) <= capacity) return true;

    const prisma_poster_preset_t fallbacks[] = {
        PRISMA_POSTER_PRESET_720,
        PRISMA_POSTER_PRESET_480,
        PRISMA_POSTER_PRESET_360,
        PRISMA_POSTER_PRESET_240,
        PRISMA_POSTER_PRESET_166,
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        if (!prisma_poster_resolve_output(fallbacks[i], aspect, sw, sh, tw, th)) continue;
        if ((size_t)(*tw) * (*th) <= capacity) {
            if (fell_back) *fell_back = true;
            return true;
        }
    }
    return false;
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

static inline uint8_t poster_index565(uint16_t c)
{
    return (uint8_t)((level5((c >> 11) & 31) << 4) |
                     (level6((c >> 5) & 63) << 2) |
                     level5(c & 31));
}

static inline uint8_t poster_index888(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rl = level5((uint16_t)(r >> 3));
    uint8_t gl = level6((uint16_t)(g >> 2));
    uint8_t bl = level5((uint16_t)(b >> 3));
    return (uint8_t)((rl << 4) | (gl << 2) | bl);
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

static void rgb565_to_index(const uint16_t *src, uint16_t sw, uint16_t sh,
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
            dst[(size_t)y * tw + x] = poster_index565(src[(size_t)sy * sw + sx]);
        }
    }
}

static void index_crop_scale(const uint8_t *src, uint16_t sw, uint16_t sh,
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

static void index_scale(const uint8_t *src, uint16_t sw, uint16_t sh,
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

static esp_jpeg_image_scale_t scale_enum(uint8_t div)
{
    switch (div) {
        case 2: return JPEG_IMAGE_SCALE_1_2;
        case 4: return JPEG_IMAGE_SCALE_1_4;
        case 8: return JPEG_IMAGE_SCALE_1_8;
        default: return JPEG_IMAGE_SCALE_0;
    }
}

static uint8_t choose_div(uint16_t sw, uint16_t sh,
                          uint16_t tw, uint16_t th,
                          size_t budget)
{
    const uint8_t ds[] = {8, 4, 2, 1};
    size_t out = (size_t)tw * th;
    uint16_t need_w = tw < sw ? tw : sw;
    uint16_t need_h = th < sh ? th : sh;
    for (size_t i = 0; i < sizeof(ds); ++i) {
        uint8_t d = ds[i];
        uint16_t w = sw / d, h = sh / d;
        if (!w || !h || w < need_w || h < need_h) continue;
        if ((size_t)w * h * 2u + out <= budget) return d;
    }
    for (size_t i = 0; i < sizeof(ds); ++i) {
        uint8_t d = ds[i];
        uint16_t w = sw / d, h = sh / d;
        if (w && h && (size_t)w * h * 2u + out <= budget) return d;
    }
    return 0;
}

static esp_err_t store_index(prisma_processed_slot_t slot,
                             const uint8_t *pixels,
                             uint16_t w, uint16_t h,
                             uint32_t generation, uint32_t hash)
{
    size_t n = (size_t)w * h;
    if (n > prisma_processed_capacity(slot)) return ESP_ERR_INVALID_SIZE;
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
    s_override_active = true;
}

static esp_err_t status_fail(esp_err_t err, const char *message)
{
    s_status.busy = false;
    s_status.valid = false;
    strlcpy(s_status.error, message ? message : "JPEG processing failed", sizeof(s_status.error));
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "%s", s_status.error);
    return err;
}

static void status_commit(uint16_t sw, uint16_t sh,
                          uint16_t w, uint16_t h,
                          uint32_t elapsed, uint32_t generation,
                          uint8_t div, size_t payload)
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

static int lowzag_read(void *ctx, uint8_t *buf, size_t len)
{
    lowzag_src_t *s = (lowzag_src_t *)ctx;
    if (len > s->left) len = s->left;
    memcpy(buf, s->p, len);
    s->p += len;
    s->left -= len;
    return (int)len;
}

static void lowzag_row(void *ctx, int y, int w, const uint8_t *rgb)
{
    lowzag_sink_t *s = (lowzag_sink_t *)ctx;
    if (!s || !s->pixels || y < 0 || y >= s->height || w != s->width) {
        if (s) s->overflow = true;
        return;
    }
    uint8_t *dst = s->pixels + (size_t)y * (size_t)s->width;
    for (int x = 0; x < w; ++x)
        dst[x] = poster_index888(rgb[x * 3], rgb[x * 3 + 1], rgb[x * 3 + 2]);
}

static void *lowzag_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void lowzag_free(void *ctx, void *p)
{
    (void)ctx;
    heap_caps_free(p);
}

static int lowzag_cancel(void *ctx)
{
    unsigned *polls = (unsigned *)ctx;
    if (polls) {
        (*polls)++;
        if (((*polls) & 31u) == 0u) taskYIELD();
    }
    return 0;
}

static esp_err_t decode_progressive(const uint8_t *jpeg, size_t jpeg_size,
                                    uint16_t target_w, uint16_t target_h,
                                    uint8_t **out_pixels,
                                    uint16_t *out_w, uint16_t *out_h,
                                    uint8_t *out_div)
{
    *out_pixels = NULL;
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= JPEG_WORKSPACE_RESERVE) return ESP_ERR_NO_MEM;
    size_t budget = free_psram - JPEG_WORKSPACE_RESERVE;

    lowzag_cfg_t cfg = LOWZAG_CFG_INIT;
    cfg.target_w = target_w;
    cfg.target_h = target_h;
    cfg.mem_limit = budget;

    lowzag_estimate_t est = LOWZAG_ESTIMATE_INIT;
    lowzag_err_t le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
    if (le != LOWZAG_OK || est.out_w <= 0 || est.out_h <= 0 ||
        est.out_w > UINT16_MAX || est.out_h > UINT16_MAX)
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;

    size_t pixel_bytes = (size_t)est.out_w * est.out_h;
    if (pixel_bytes >= budget) return ESP_ERR_NO_MEM;
    cfg.mem_limit = budget - pixel_bytes;

    est = (lowzag_estimate_t)LOWZAG_ESTIMATE_INIT;
    le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
    if (le != LOWZAG_OK)
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;

    pixel_bytes = (size_t)est.out_w * est.out_h;
    uint8_t *pixels = psram_alloc(pixel_bytes);
    if (!pixels) return ESP_ERR_NO_MEM;

    void *state = heap_caps_aligned_calloc(lowzag_state_align(), 1, lowzag_state_size(),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!state) {
        free(pixels);
        return ESP_ERR_NO_MEM;
    }

    lowzag_src_t src = {.p = jpeg, .left = jpeg_size};
    lowzag_sink_t sink = {
        .pixels = pixels,
        .width = est.out_w,
        .height = est.out_h,
        .overflow = false,
    };
    unsigned polls = 0;
    cfg.read = lowzag_read;
    cfg.read_ctx = &src;
    cfg.row = lowzag_row;
    cfg.row_ctx = &sink;
    cfg.alloc = lowzag_alloc;
    cfg.free = lowzag_free;
    cfg.cancel = lowzag_cancel;
    cfg.cancel_ctx = &polls;
    cfg.state = state;

    lowzag_info_t info = LOWZAG_INFO_INIT;
    le = lowzag_decode(&cfg, &info);
    heap_caps_free(state);
    if (le != LOWZAG_OK || sink.overflow ||
        info.out_w != est.out_w || info.out_h != est.out_h) {
        free(pixels);
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }

    *out_pixels = pixels;
    *out_w = (uint16_t)info.out_w;
    *out_h = (uint16_t)info.out_h;
    *out_div = (uint8_t)info.scale;
    return ESP_OK;
}

static esp_err_t build_baseline_override(const uint8_t *jpeg, size_t jpeg_size,
                                         uint16_t sw, uint16_t sh,
                                         uint16_t tw, uint16_t th,
                                         uint32_t generation, uint32_t cfg_hash,
                                         uint8_t *used_div)
{
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram > JPEG_WORKSPACE_RESERVE ? free_psram - JPEG_WORKSPACE_RESERVE : 0;
    uint8_t div = choose_div(sw, sh, tw, th, budget);
    if (!div) return ESP_ERR_NO_MEM;

    uint16_t dw = sw / div, dh = sh / div;
    size_t decode_bytes = (size_t)dw * dh * sizeof(uint16_t);
    uint16_t *decode = psram_alloc(decode_bytes);
    uint8_t *index = psram_alloc((size_t)tw * th);
    if (!decode || !index) {
        free(decode); free(index);
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_size,
        .outbuf = (uint8_t *)decode,
        .outbuf_size = decode_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale_enum(div),
        .flags = {.swap_color_bytes = 0},
    };
    esp_jpeg_image_output_t decoded = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &decoded);
    if (err == ESP_OK) {
        rgb565_to_index(decode, decoded.width, decoded.height, index, tw, th);
        err = store_index(PRISMA_PROCESSED_IMAGE_OUT, index, tw, th, generation, cfg_hash);
    }
    free(decode);
    free(index);
    if (err == ESP_OK && used_div) *used_div = div;
    return err;
}

static esp_err_t build_preview_from_output(uint16_t sw, uint16_t sh,
                                           prisma_poster_aspect_t aspect,
                                           uint16_t tw, uint16_t th,
                                           uint32_t generation, uint32_t cfg_hash,
                                           uint16_t *pw_out, uint16_t *ph_out)
{
    uint16_t pw = 0, ph = 0;
    if (!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166, aspect, sw, sh, &pw, &ph))
        return ESP_ERR_INVALID_ARG;

    size_t main_bytes = (size_t)tw * th;
    uint8_t *main = psram_alloc(main_bytes);
    uint8_t *small = psram_alloc((size_t)pw * ph);
    if (!main || !small) {
        free(main); free(small);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = prisma_processed_read(PRISMA_PROCESSED_IMAGE_OUT, 0, main, main_bytes);
    if (err == ESP_OK) {
        index_scale(main, tw, th, small, pw, ph);
        err = store_index(PRISMA_PROCESSED_IMAGE_PREVIEW, small, pw, ph, generation, cfg_hash);
    }
    free(main);
    free(small);
    if (err == ESP_OK) {
        if (pw_out) *pw_out = pw;
        if (ph_out) *ph_out = ph;
    }
    return err;
}

static esp_err_t process_override(const uint8_t *jpeg, size_t jpeg_size,
                                  jpeg_kind_t kind,
                                  uint16_t sw, uint16_t sh,
                                  const prisma_poster_config_t *pcfg,
                                  uint32_t generation, uint32_t cfg_hash,
                                  uint16_t tw, uint16_t th,
                                  bool capacity_fallback)
{
    status_begin();
    int64_t started = esp_timer_get_time();

    prisma_processed_info_t cached = {0}, preview = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &cached);
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_PREVIEW, &preview);
    if (cached.valid && preview.valid &&
        cached.source_generation == generation && preview.source_generation == generation &&
        cached.config_hash == cfg_hash && preview.config_hash == cfg_hash) {
        status_commit(sw, sh, cached.width, cached.height, 0, generation, 1, cached.payload_size);
        return ESP_OK;
    }

    esp_err_t err = ESP_FAIL;
    uint8_t div = 1;

    if (kind == JPEG_KIND_BASELINE) {
        err = build_baseline_override(jpeg, jpeg_size, sw, sh, tw, th,
                                      generation, cfg_hash, &div);
        if (err == ESP_ERR_NO_MEM && !capacity_fallback && (tw > 720 || th > 720)) {
            if (prisma_poster_resolve_output(PRISMA_POSTER_PRESET_720, pcfg->image_aspect,
                                             sw, sh, &tw, &th)) {
                ESP_LOGW(TAG, "IMAGE native target exceeds safe PSRAM; fallback to %ux%u", tw, th);
                err = build_baseline_override(jpeg, jpeg_size, sw, sh, tw, th,
                                              generation, cfg_hash, &div);
            }
        }
    } else {
        uint8_t *decoded = NULL;
        uint16_t dw = 0, dh = 0;
        err = decode_progressive(jpeg, jpeg_size, tw, th, &decoded, &dw, &dh, &div);
        if (err == ESP_ERR_NO_MEM && !capacity_fallback && (tw > 720 || th > 720)) {
            if (prisma_poster_resolve_output(PRISMA_POSTER_PRESET_720, pcfg->image_aspect,
                                             sw, sh, &tw, &th)) {
                ESP_LOGW(TAG, "progressive IMAGE native target exceeds safe PSRAM; fallback to %ux%u", tw, th);
                err = decode_progressive(jpeg, jpeg_size, tw, th, &decoded, &dw, &dh, &div);
            }
        }
        if (err == ESP_OK) {
            uint8_t *index = psram_alloc((size_t)tw * th);
            if (!index) {
                free(decoded);
                return status_fail(ESP_ERR_NO_MEM, "not enough PSRAM for IMAGE output");
            }
            index_crop_scale(decoded, dw, dh, index, tw, th);
            free(decoded);
            err = store_index(PRISMA_PROCESSED_IMAGE_OUT, index, tw, th, generation, cfg_hash);
            free(index);
        }
    }

    if (err != ESP_OK)
        return status_fail(err, err == ESP_ERR_NO_MEM ?
                           "IMAGE exceeds safe PSRAM workspace" :
                           "could not build IMAGE OUT");

    uint16_t pw = 0, ph = 0;
    err = build_preview_from_output(sw, sh, pcfg->image_aspect, tw, th,
                                    generation, cfg_hash, &pw, &ph);
    if (err != ESP_OK)
        return status_fail(err, "could not build IMAGE PREVIEW 166");

    prisma_processed_info_t out = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &out);
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    status_commit(sw, sh, tw, th, elapsed, generation, div, out.payload_size);
    ESP_LOGI(TAG, "IMAGE %s source=%ux%u output=%ux%u preview=%ux%u bytes=%u%s",
             kind == JPEG_KIND_PROGRESSIVE ? "progressive" : "baseline-adaptive",
             sw, sh, tw, th, pw, ph, (unsigned)out.payload_size,
             capacity_fallback ? " capacity-fallback" : "");
    return ESP_OK;
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static esp_err_t send_index_bmp(httpd_req_t *req, prisma_processed_slot_t slot)
{
    prisma_processed_info_t in = {0};
    esp_err_t err = prisma_processed_get_info(slot, &in);
    if (err != ESP_OK || !in.valid || in.format != PRISMA_PROCESSED_FMT_INDEX8)
        return ESP_ERR_NOT_FOUND;

    uint16_t w = in.width, h = in.height;
    uint32_t po = 54u + 64u * 4u;
    uint32_t rowb = ((uint32_t)w + 3u) & ~3u;
    uint32_t img = rowb * h;
    uint32_t file = po + img;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    le32(&hdr[2], file); le32(&hdr[10], po); le32(&hdr[14], 40);
    le32(&hdr[18], w); le32(&hdr[22], h);
    le16(&hdr[26], 1); le16(&hdr[28], 8);
    le32(&hdr[34], img); le32(&hdr[46], 64); le32(&hdr[50], 64);

    static const uint8_t rb[4] = {0, 82, 173, 255};
    static const uint8_t gg[4] = {0, 85, 170, 255};
    uint8_t pal[256];
    for (int i = 0; i < 64; ++i) {
        uint8_t r = (i >> 4) & 3, g = (i >> 2) & 3, b = i & 3;
        pal[i * 4] = rb[b]; pal[i * 4 + 1] = gg[g];
        pal[i * 4 + 2] = rb[r]; pal[i * 4 + 3] = 0;
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
        s_override_active = false;
        return __real_prisma_process_run_poster();
    }

    uint8_t *jpeg = psram_alloc(image.size);
    if (!jpeg) {
        s_override_active = false;
        return __real_prisma_process_run_poster();
    }
    esp_err_t err = prisma_image_ab_read(0, jpeg, image.size);
    if (err != ESP_OK) {
        free(jpeg);
        s_override_active = false;
        return __real_prisma_process_run_poster();
    }

    uint16_t sw = 0, sh = 0;
    jpeg_kind_t kind = jpeg_probe(jpeg, image.size, &sw, &sh);
    if (kind == JPEG_KIND_UNKNOWN) {
        free(jpeg);
        s_override_active = false;
        return __real_prisma_process_run_poster();
    }

    prisma_poster_config_t pcfg;
    (void)prisma_poster_config_load(&pcfg);
    uint32_t cfg_hash = prisma_poster_config_hash(&pcfg, false);

    uint16_t tw = 0, th = 0;
    bool capacity_fallback = false;
    if (!resolve_fitted(pcfg.image_preset, pcfg.image_aspect, sw, sh,
                        prisma_processed_capacity(PRISMA_PROCESSED_IMAGE_OUT),
                        &tw, &th, &capacity_fallback)) {
        free(jpeg);
        status_begin();
        return status_fail(ESP_ERR_INVALID_SIZE, "IMAGE output cannot fit flash partition");
    }

    if (capacity_fallback)
        ESP_LOGW(TAG, "IMAGE native output %ux%u cannot fit flash; fallback target=%ux%u",
                 sw, sh, tw, th);

    if (kind == JPEG_KIND_BASELINE && !capacity_fallback) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t budget = free_psram > JPEG_WORKSPACE_RESERVE ? free_psram - JPEG_WORKSPACE_RESERVE : 0;
        if (choose_div(sw, sh, tw, th, budget) != 0) {
            free(jpeg);
            s_override_active = false;
            return __real_prisma_process_run_poster();
        }
        if (tw <= 720 && th <= 720) {
            free(jpeg);
            s_override_active = false;
            return __real_prisma_process_run_poster();
        }
        if (!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_720, pcfg.image_aspect,
                                          sw, sh, &tw, &th)) {
            free(jpeg);
            status_begin();
            return status_fail(ESP_ERR_INVALID_ARG, "invalid IMAGE 720 fallback");
        }
    }

    err = process_override(jpeg, image.size, kind, sw, sh, &pcfg,
                           image.generation, cfg_hash, tw, th, capacity_fallback);
    free(jpeg);
    return err;
}

esp_err_t __wrap_prisma_process_get_status(prisma_process_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_override_active) return __real_prisma_process_get_status(out);
    *out = s_status;
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

esp_err_t __wrap_prisma_process_send_bmp(httpd_req_t *req)
{
    if (!s_override_active) return __real_prisma_process_send_bmp(req);
    if (!s_status.valid) return ESP_ERR_NOT_FOUND;
    return send_index_bmp(req, PRISMA_PROCESSED_IMAGE_OUT);
}

esp_err_t __wrap_prisma_process_run_lines(void)
{
    s_override_active = false;
    return __real_prisma_process_run_lines();
}

esp_err_t __wrap_prisma_process_run_rectangles(void)
{
    s_override_active = false;
    return __real_prisma_process_run_rectangles();
}

esp_err_t __wrap_prisma_process_run_circles(void)
{
    s_override_active = false;
    return __real_prisma_process_run_circles();
}

esp_err_t __wrap_prisma_process_run_triangles(void)
{
    s_override_active = false;
    return __real_prisma_process_run_triangles();
}
