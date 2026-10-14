#include "video_process.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lowzag.h"

#include "poster_config.h"
#include "processed_store.h"
#include "video_ab.h"

#define MAX_SOURCE_FRAMES 180
#define SOURCE_FRAME_MS 200u
#define VIDEO_PSRAM_RESERVE (1536u * 1024u)
#define SCAN_CHUNK 1024u
#define RLE_CHUNK 512u
#define FRAME_ENC_RAW 0u
#define FRAME_ENC_RLE 1u

static const char *TAG = "PRISMA_VIDEO_ADAPT";

typedef struct {
    size_t offset;
    size_t length;
} frame_ref_t;

typedef struct __attribute__((packed)) {
    uint32_t length;
    uint8_t encoding;
    uint8_t reserved[3];
} frame_record_t;

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

static SemaphoreHandle_t g_lock;
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
    g_status.prepared = false;
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

static jpeg_kind_t jpeg_probe(const uint8_t *buf, size_t len,
                              uint16_t *width, uint16_t *height)
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

static esp_err_t scan_frames(size_t source_size,
                             frame_ref_t refs[MAX_SOURCE_FRAMES],
                             uint16_t *count)
{
    uint8_t buf[SCAN_CHUNK];
    size_t off = 0, soi = SIZE_MAX;
    uint8_t prev = 0;
    bool have = false;
    uint16_t n = 0;

    while (off < source_size) {
        size_t chunk = source_size - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        esp_err_t err = prisma_video_ab_read(off, buf, chunk);
        if (err != ESP_OK) return err;

        for (size_t i = 0; i < chunk; ++i) {
            uint8_t b = buf[i];
            size_t pos = off + i;
            if (have) {
                if (soi == SIZE_MAX && prev == 0xFF && b == 0xD8) {
                    soi = pos - 1;
                } else if (soi != SIZE_MAX && prev == 0xFF && b == 0xD9) {
                    if (n >= MAX_SOURCE_FRAMES) {
                        *count = n;
                        return ESP_OK;
                    }
                    refs[n].offset = soi;
                    refs[n].length = (pos + 1) - soi;
                    n++;
                    soi = SIZE_MAX;
                }
            }
            prev = b;
            have = true;
        }
        off += chunk;
    }
    *count = n;
    return ESP_OK;
}

static esp_err_t first_frame_info(const frame_ref_t *r,
                                  uint16_t *w, uint16_t *h,
                                  jpeg_kind_t *kind)
{
    uint8_t *buf = psram_alloc(r->length);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t err = prisma_video_ab_read(r->offset, buf, r->length);
    if (err == ESP_OK) {
        *kind = jpeg_probe(buf, r->length, w, h);
        if (*kind == JPEG_KIND_UNKNOWN) err = ESP_ERR_NOT_SUPPORTED;
    }
    free(buf);
    return err;
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
    return (uint8_t)((level5(r >> 3) << 4) |
                     (level6(g >> 2) << 2) |
                     level5(b >> 3));
}

static void crop_for_target(uint16_t sw, uint16_t sh,
                            uint16_t tw, uint16_t th,
                            uint16_t *x0, uint16_t *y0,
                            uint16_t *cw, uint16_t *ch)
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

static void index_to_index(const uint8_t *src, uint16_t sw, uint16_t sh,
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
    uint16_t need_w = tw < sw ? tw : sw;
    uint16_t need_h = th < sh ? th : sh;
    for (size_t i = 0; i < sizeof(ds); ++i) {
        uint8_t d = ds[i];
        uint16_t w = sw / d, h = sh / d;
        if (!w || !h || w < need_w || h < need_h) continue;
        if ((size_t)w * h * 2u <= budget) return d;
    }
    for (size_t i = 0; i < sizeof(ds); ++i) {
        uint8_t d = ds[i];
        uint16_t w = sw / d, h = sh / d;
        if (w && h && (size_t)w * h * 2u <= budget) return d;
    }
    return 0;
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
    uint8_t *dst = s->pixels + (size_t)y * s->width;
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

static esp_err_t decode_progressive_index(const uint8_t *jpeg, size_t jpeg_size,
                                          uint16_t tw, uint16_t th,
                                          uint8_t *index)
{
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= VIDEO_PSRAM_RESERVE) return ESP_ERR_NO_MEM;
    size_t budget = free_psram - VIDEO_PSRAM_RESERVE;

    lowzag_cfg_t cfg = LOWZAG_CFG_INIT;
    cfg.target_w = tw;
    cfg.target_h = th;
    cfg.mem_limit = budget;

    lowzag_estimate_t est = LOWZAG_ESTIMATE_INIT;
    lowzag_err_t le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
    if (le != LOWZAG_OK || est.out_w <= 0 || est.out_h <= 0 ||
        est.out_w > UINT16_MAX || est.out_h > UINT16_MAX)
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;

    size_t tmp_bytes = (size_t)est.out_w * est.out_h;
    if (tmp_bytes >= budget) return ESP_ERR_NO_MEM;
    cfg.mem_limit = budget - tmp_bytes;

    est = (lowzag_estimate_t)LOWZAG_ESTIMATE_INIT;
    le = lowzag_estimate(&cfg, jpeg, jpeg_size, &est);
    if (le != LOWZAG_OK)
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;

    tmp_bytes = (size_t)est.out_w * est.out_h;
    uint8_t *tmp = psram_alloc(tmp_bytes);
    if (!tmp) return ESP_ERR_NO_MEM;
    void *state = heap_caps_aligned_calloc(lowzag_state_align(), 1, lowzag_state_size(),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!state) {
        free(tmp);
        return ESP_ERR_NO_MEM;
    }

    lowzag_src_t src = {.p = jpeg, .left = jpeg_size};
    lowzag_sink_t sink = {
        .pixels = tmp,
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
        free(tmp);
        return le == LOWZAG_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }

    index_to_index(tmp, (uint16_t)info.out_w, (uint16_t)info.out_h, index, tw, th);
    free(tmp);
    return ESP_OK;
}

static esp_err_t decode_baseline_index(const uint8_t *jpeg, size_t jpeg_size,
                                       uint16_t sw, uint16_t sh,
                                       uint16_t tw, uint16_t th,
                                       uint8_t *index)
{
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram > VIDEO_PSRAM_RESERVE ? free_psram - VIDEO_PSRAM_RESERVE : 0;
    uint8_t div = choose_div(sw, sh, tw, th, budget);
    if (!div) return ESP_ERR_NO_MEM;

    uint16_t dw = sw / div, dh = sh / div;
    size_t decode_bytes = (size_t)dw * dh * 2u;
    uint16_t *decode = psram_alloc(decode_bytes);
    if (!decode) return ESP_ERR_NO_MEM;

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_size,
        .outbuf = (uint8_t *)decode,
        .outbuf_size = decode_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale_enum(div),
        .flags = {.swap_color_bytes = 0},
    };
    esp_jpeg_image_output_t d = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &d);
    if (err == ESP_OK) rgb565_to_index(decode, d.width, d.height, index, tw, th);
    free(decode);
    return err;
}

static esp_err_t decode_frame_index(const uint8_t *jpeg, size_t jpeg_size,
                                    uint16_t tw, uint16_t th,
                                    uint8_t *index)
{
    uint16_t sw = 0, sh = 0;
    jpeg_kind_t kind = jpeg_probe(jpeg, jpeg_size, &sw, &sh);
    if (kind == JPEG_KIND_BASELINE)
        return decode_baseline_index(jpeg, jpeg_size, sw, sh, tw, th, index);
    if (kind == JPEG_KIND_PROGRESSIVE)
        return decode_progressive_index(jpeg, jpeg_size, tw, th, index);
    return ESP_ERR_NOT_SUPPORTED;
}

static size_t rle_size(const uint8_t *p, size_t n)
{
    size_t out = 0, i = 0;
    while (i < n) {
        uint8_t v = p[i];
        size_t run = 1;
        while (i + run < n && p[i + run] == v && run < 255) run++;
        out += 2;
        i += run;
    }
    return out;
}

static esp_err_t write_rle(prisma_processed_slot_t slot,
                           size_t *off,
                           const uint8_t *p, size_t n)
{
    uint8_t buf[RLE_CHUNK];
    size_t used = 0, i = 0;
    while (i < n) {
        uint8_t v = p[i];
        size_t run = 1;
        while (i + run < n && p[i + run] == v && run < 255) run++;
        if (used + 2 > sizeof(buf)) {
            esp_err_t e = prisma_processed_write(slot, *off, buf, used);
            if (e != ESP_OK) return e;
            *off += used;
            used = 0;
        }
        buf[used++] = (uint8_t)run;
        buf[used++] = v;
        i += run;
    }
    if (used) {
        esp_err_t e = prisma_processed_write(slot, *off, buf, used);
        if (e != ESP_OK) return e;
        *off += used;
    }
    return ESP_OK;
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
    if ((size_t)(*tw) * (*th) + sizeof(frame_record_t) <= capacity) return true;

    const prisma_poster_preset_t fallbacks[] = {
        PRISMA_POSTER_PRESET_720,
        PRISMA_POSTER_PRESET_480,
        PRISMA_POSTER_PRESET_360,
        PRISMA_POSTER_PRESET_240,
        PRISMA_POSTER_PRESET_166,
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        if (!prisma_poster_resolve_output(fallbacks[i], aspect, sw, sh, tw, th)) continue;
        if ((size_t)(*tw) * (*th) + sizeof(frame_record_t) <= capacity) {
            if (fell_back) *fell_back = true;
            return true;
        }
    }
    return false;
}

static esp_err_t build_slot(prisma_processed_slot_t slot,
                            const frame_ref_t *refs, uint16_t count,
                            uint16_t tw, uint16_t th,
                            uint32_t generation, uint32_t cfg_hash,
                            uint16_t *stored_out, uint32_t *interval_out)
{
    if (!count) return ESP_ERR_NOT_FOUND;
    size_t raw = (size_t)tw * th;
    size_t cap = prisma_processed_capacity(slot);
    if (!raw || cap <= sizeof(frame_record_t) || raw + sizeof(frame_record_t) > cap)
        return ESP_ERR_INVALID_SIZE;

    size_t max_frames = cap / (raw + sizeof(frame_record_t));
    if (max_frames < 1) return ESP_ERR_INVALID_SIZE;
    if (max_frames > count) max_frames = count;
    uint16_t stride = (uint16_t)((count + max_frames - 1) / max_frames);

    size_t max_jpeg = 0;
    for (uint16_t i = 0; i < count; i = (uint16_t)(i + stride))
        if (refs[i].length > max_jpeg) max_jpeg = refs[i].length;

    uint8_t *jpeg = psram_alloc(max_jpeg);
    uint8_t *index = psram_alloc(raw);
    if (!jpeg || !index) {
        free(jpeg); free(index);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = prisma_processed_begin(slot, PRISMA_PROCESSED_FMT_VIDEO_INDEX8,
                                           generation, cfg_hash, tw, th);
    if (err != ESP_OK) {
        free(jpeg); free(index);
        return err;
    }

    size_t outoff = 0;
    uint16_t stored = 0;
    for (uint16_t fi = 0; fi < count && err == ESP_OK; fi = (uint16_t)(fi + stride)) {
        frame_ref_t r = refs[fi];
        if (r.length > max_jpeg) { err = ESP_ERR_INVALID_SIZE; break; }
        err = prisma_video_ab_read(r.offset, jpeg, r.length);
        if (err != ESP_OK) break;

        err = decode_frame_index(jpeg, r.length, tw, th, index);
        if (err != ESP_OK) break;

        size_t rs = rle_size(index, raw);
        frame_record_t rec = {
            .length = (uint32_t)(rs < raw ? rs : raw),
            .encoding = (uint8_t)(rs < raw ? FRAME_ENC_RLE : FRAME_ENC_RAW),
            .reserved = {0, 0, 0},
        };
        if (outoff + sizeof(rec) + rec.length > cap) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = prisma_processed_write(slot, outoff, &rec, sizeof(rec));
        if (err != ESP_OK) break;
        outoff += sizeof(rec);
        if (rec.encoding == FRAME_ENC_RAW) {
            err = prisma_processed_write(slot, outoff, index, raw);
            if (err == ESP_OK) outoff += raw;
        } else {
            err = write_rle(slot, &outoff, index, raw);
        }
        if (err == ESP_OK) stored++;
    }

    free(jpeg);
    free(index);

    if (err == ESP_OK) {
        uint32_t interval = SOURCE_FRAME_MS * stride;
        err = prisma_processed_finalize(slot, stored, count, interval);
        if (err == ESP_OK) {
            *stored_out = stored;
            *interval_out = interval;
        }
    }
    if (err != ESP_OK) prisma_processed_abort(slot);
    return err;
}

static void status_from_cache(const prisma_video_info_t *src,
                              const prisma_processed_info_t *main,
                              const prisma_processed_info_t *prev,
                              uint16_t sw, uint16_t sh,
                              uint32_t elapsed)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.prepared = true;
    g_status.valid = true;
    g_status.source_generation = src->generation;
    g_status.source_bytes = src->size;
    g_status.source_frame_count = main->source_frame_count;
    g_status.frame_count = main->frame_count;
    g_status.preview_frame_count = prev->frame_count;
    g_status.source_width = sw;
    g_status.source_height = sh;
    g_status.width = main->width;
    g_status.height = main->height;
    g_status.preview_width = prev->width;
    g_status.preview_height = prev->height;
    g_status.frame_interval_ms = main->frame_interval_ms;
    g_status.preview_interval_ms = prev->frame_interval_ms;
    g_status.last_process_ms = elapsed;
    g_status.output_bytes = main->payload_size;
    g_status.preview_bytes = prev->payload_size;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
}

esp_err_t prisma_video_process_prepare(void)
{
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    int64_t started = esp_timer_get_time();

    prisma_video_info_t src = {0};
    if (prisma_video_ab_get_info(&src) != ESP_OK || !src.valid || !src.size) {
        set_error("no active VIDEO in A/B storage");
        return ESP_ERR_NOT_FOUND;
    }

    frame_ref_t refs[MAX_SOURCE_FRAMES];
    uint16_t count = 0;
    esp_err_t err = scan_frames(src.size, refs, &count);
    if (err != ESP_OK || !count) {
        set_error("VIDEO payload has no complete JPEG frames");
        return err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t sw = 0, sh = 0;
    jpeg_kind_t first_kind = JPEG_KIND_UNKNOWN;
    err = first_frame_info(&refs[0], &sw, &sh, &first_kind);
    if (err != ESP_OK) {
        set_error("VIDEO frames are not supported JPEG");
        return err;
    }

    prisma_poster_config_t cfg;
    (void)prisma_poster_config_load(&cfg);
    uint32_t hash = prisma_poster_config_hash(&cfg, true);

    prisma_processed_info_t main = {0}, prev = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT, &main);
    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW, &prev);
    if (main.valid && prev.valid &&
        main.source_generation == src.generation && prev.source_generation == src.generation &&
        main.config_hash == hash && prev.config_hash == hash) {
        status_from_cache(&src, &main, &prev, sw, sh, 0);
        return ESP_OK;
    }

    uint16_t tw = 0, th = 0, pw = 0, ph = 0;
    bool capacity_fallback = false;
    if (!resolve_fitted(cfg.video_preset, cfg.video_aspect, sw, sh,
                        prisma_processed_capacity(PRISMA_PROCESSED_VIDEO_OUT),
                        &tw, &th, &capacity_fallback) ||
        !prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166, cfg.video_aspect,
                                      sw, sh, &pw, &ph)) {
        set_error("invalid VIDEO output configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (capacity_fallback)
        ESP_LOGW(TAG, "VIDEO native output cannot fit flash; fallback target=%ux%u", tw, th);

    uint16_t stored = 0, pstored = 0;
    uint32_t interval = 0, pinterval = 0;
    err = build_slot(PRISMA_PROCESSED_VIDEO_OUT, refs, count, tw, th,
                     src.generation, hash, &stored, &interval);

    if ((err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_SIZE) &&
        !capacity_fallback && (tw > 720 || th > 720)) {
        uint16_t fw = 0, fh = 0;
        if (prisma_poster_resolve_output(PRISMA_POSTER_PRESET_720, cfg.video_aspect,
                                         sw, sh, &fw, &fh)) {
            ESP_LOGW(TAG, "VIDEO native target %ux%u exceeds safe workspace; fallback to %ux%u",
                     tw, th, fw, fh);
            tw = fw; th = fh;
            err = build_slot(PRISMA_PROCESSED_VIDEO_OUT, refs, count, tw, th,
                             src.generation, hash, &stored, &interval);
        }
    }

    if (err != ESP_OK) {
        set_error(err == ESP_ERR_NO_MEM ?
                  "not enough PSRAM workspace for VIDEO" :
                  "could not build VIDEO OUT");
        return err;
    }

    err = build_slot(PRISMA_PROCESSED_VIDEO_PREVIEW, refs, count, pw, ph,
                     src.generation, hash, &pstored, &pinterval);
    if (err != ESP_OK) {
        set_error("could not build VIDEO PREVIEW 166");
        return err;
    }

    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT, &main);
    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW, &prev);
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    status_from_cache(&src, &main, &prev, sw, sh, elapsed);
    ESP_LOGI(TAG, "VIDEO %s source=%ux%u frames=%u output=%ux%u/%u bytes=%u preview=%ux%u/%u %lums",
             first_kind == JPEG_KIND_PROGRESSIVE ? "progressive" : "baseline",
             sw, sh, count, tw, th, stored, (unsigned)main.payload_size,
             pw, ph, pstored, (unsigned long)elapsed);
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

static esp_err_t load_frame(prisma_processed_slot_t slot,
                            uint16_t index,
                            uint8_t **pixels,
                            uint16_t *w, uint16_t *h)
{
    prisma_processed_info_t in = {0};
    esp_err_t err = prisma_processed_get_info(slot, &in);
    if (err != ESP_OK || !in.valid || in.format != PRISMA_PROCESSED_FMT_VIDEO_INDEX8)
        return ESP_ERR_NOT_FOUND;
    if (index >= in.frame_count) return ESP_ERR_INVALID_ARG;

    size_t off = 0;
    frame_record_t rec = {0};
    for (uint16_t i = 0; i <= index; ++i) {
        err = prisma_processed_read(slot, off, &rec, sizeof(rec));
        if (err != ESP_OK) return err;
        off += sizeof(rec);
        if (rec.length > in.payload_size - off) return ESP_ERR_INVALID_SIZE;
        if (i < index) off += rec.length;
    }

    size_t n = (size_t)in.width * in.height;
    uint8_t *out = psram_alloc(n);
    if (!out) return ESP_ERR_NO_MEM;

    if (rec.encoding == FRAME_ENC_RAW) {
        if (rec.length != n) { free(out); return ESP_ERR_INVALID_SIZE; }
        err = prisma_processed_read(slot, off, out, n);
    } else if (rec.encoding == FRAME_ENC_RLE) {
        uint8_t buf[RLE_CHUNK];
        size_t pos = 0, written = 0;
        while (pos < rec.length && err == ESP_OK) {
            size_t k = rec.length - pos;
            if (k > sizeof(buf)) k = sizeof(buf);
            if (k & 1u) k--;
            if (!k) { err = ESP_ERR_INVALID_SIZE; break; }
            err = prisma_processed_read(slot, off + pos, buf, k);
            if (err != ESP_OK) break;
            for (size_t j = 0; j < k; j += 2) {
                uint8_t run = buf[j], v = buf[j + 1];
                if (!run || written + run > n) { err = ESP_ERR_INVALID_SIZE; break; }
                memset(out + written, v, run);
                written += run;
            }
            pos += k;
        }
        if (err == ESP_OK && written != n) err = ESP_ERR_INVALID_SIZE;
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    if (err != ESP_OK) {
        free(out);
        return err;
    }
    *pixels = out;
    *w = in.width;
    *h = in.height;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status.last_frame = index;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
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

esp_err_t prisma_video_process_send_indexed(httpd_req_t *req,
                                            uint16_t index,
                                            bool preview)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    uint8_t *p = NULL;
    uint16_t w = 0, h = 0;
    esp_err_t err = load_frame(preview ? PRISMA_PROCESSED_VIDEO_PREVIEW : PRISMA_PROCESSED_VIDEO_OUT,
                               index, &p, &w, &h);
    if (err != ESP_OK) return err;
    uint8_t head[4];
    le16(head, w); le16(head + 2, h);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Prisma-Format", "POSTER64-INDEX8");
    err = httpd_resp_send_chunk(req, (const char *)head, 4);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)p, (size_t)w * h);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    free(p);
    return err;
}

esp_err_t prisma_video_process_send_bmp(httpd_req_t *req,
                                        uint16_t index,
                                        bool preview)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    uint8_t *p = NULL;
    uint16_t w = 0, h = 0;
    esp_err_t err = load_frame(preview ? PRISMA_PROCESSED_VIDEO_PREVIEW : PRISMA_PROCESSED_VIDEO_OUT,
                               index, &p, &w, &h);
    if (err != ESP_OK) return err;

    uint32_t po = 54u + 256u;
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
    if (!row) { free(p); return ESP_ERR_NO_MEM; }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)pal, sizeof(pal));
    for (int y = h - 1; err == ESP_OK && y >= 0; --y) {
        memset(row, 0, rowb);
        memcpy(row, p + (size_t)y * w, w);
        err = httpd_resp_send_chunk(req, (const char *)row, rowb);
    }
    free(row);
    free(p);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}
