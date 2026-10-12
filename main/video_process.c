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
#define VIDEO_TARGET_LONG_SIDE 640u
#define VIDEO_PSRAM_RESERVE (2u * 1024u * 1024u)

typedef struct {
    size_t offset;
    size_t length;
} frame_ref_t;

static SemaphoreHandle_t g_lock;
static uint8_t *g_video;
static size_t g_video_size;
static frame_ref_t g_frames[MAX_TEST_FRAMES];
static uint16_t g_frame_count;
static uint16_t *g_decode;
static uint8_t *g_index;
static size_t g_capacity_pixels;
static uint8_t g_min_div = 1;
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

static bool valid_div(uint8_t div)
{
    return div == 1 || div == 2 || div == 4 || div == 8;
}

static uint8_t choose_scale(uint16_t width, uint16_t height)
{
    const uint8_t divs[] = {1, 2, 4, 8};
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram > VIDEO_PSRAM_RESERVE ? free_psram - VIDEO_PSRAM_RESERVE : 0;
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint8_t div = divs[i];
        uint32_t w = width / div;
        uint32_t h = height / div;
        size_t pixels = (size_t)w * h;
        size_t working = pixels * (sizeof(uint16_t) + sizeof(uint8_t));
        uint32_t long_side = w > h ? w : h;
        if (w && h && long_side <= VIDEO_TARGET_LONG_SIDE && working <= budget) return div;
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

static inline uint8_t poster_index(uint16_t c)
{
    uint8_t rl = level5((c >> 11) & 0x1F);
    uint8_t gl = level6((c >> 5) & 0x3F);
    uint8_t bl = level5(c & 0x1F);
    return (uint8_t)((rl << 4) | (gl << 2) | bl);
}

static void release_working_set(void)
{
    free(g_video);
    free(g_decode);
    free(g_index);
    g_video = NULL;
    g_decode = NULL;
    g_index = NULL;
    g_video_size = 0;
    g_frame_count = 0;
    g_capacity_pixels = 0;
    g_min_div = 1;
}

esp_err_t prisma_video_process_prepare(void)
{
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    release_working_set();
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    xSemaphoreGive(g_lock);

    prisma_video_info_t info = {0};
    if (prisma_video_ab_get_info(&info) != ESP_OK || !info.valid || info.size == 0) {
        set_error("no active VIDEO in A/B storage");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *video = psram_alloc(info.size);
    if (!video) {
        set_error("not enough PSRAM for VIDEO source");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = prisma_video_ab_read(0, video, info.size);
    if (err != ESP_OK) {
        free(video);
        set_error("could not read active VIDEO from flash");
        return err;
    }

    frame_ref_t refs[MAX_TEST_FRAMES];
    uint16_t count = 0;
    size_t soi = SIZE_MAX;
    for (size_t i = 0; i + 1 < info.size; ++i) {
        if (soi == SIZE_MAX && video[i] == 0xFF && video[i + 1] == 0xD8) {
            soi = i;
            i++;
            continue;
        }
        if (soi != SIZE_MAX && video[i] == 0xFF && video[i + 1] == 0xD9) {
            if (count >= MAX_TEST_FRAMES) break;
            refs[count].offset = soi;
            refs[count].length = (i + 2) - soi;
            count++;
            soi = SIZE_MAX;
            i++;
        }
    }

    if (count == 0) {
        free(video);
        set_error("VIDEO test payload has no complete JPEG frames");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t src_w = 0, src_h = 0;
    if (!baseline_size(video + refs[0].offset, refs[0].length, &src_w, &src_h)) {
        free(video);
        set_error("VIDEO frames must be baseline JPEG");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t div = choose_scale(src_w, src_h);
    if (!div) {
        free(video);
        set_error("VIDEO resolution exceeds adaptive PSRAM budget");
        return ESP_ERR_NO_MEM;
    }

    uint16_t out_w = src_w / div;
    uint16_t out_h = src_h / div;
    size_t pixels = (size_t)out_w * out_h;
    uint16_t *decode = psram_alloc(pixels * sizeof(uint16_t));
    uint8_t *index = psram_alloc(pixels);
    while ((!decode || !index) && div < 8) {
        free(decode);
        free(index);
        decode = NULL;
        index = NULL;
        div *= 2;
        out_w = src_w / div;
        out_h = src_h / div;
        pixels = (size_t)out_w * out_h;
        decode = psram_alloc(pixels * sizeof(uint16_t));
        index = psram_alloc(pixels);
    }
    if (!decode || !index) {
        free(video);
        free(decode);
        free(index);
        set_error("not enough PSRAM for adaptive VIDEO buffers");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_video = video;
    g_video_size = info.size;
    memcpy(g_frames, refs, count * sizeof(frame_ref_t));
    g_frame_count = count;
    g_decode = decode;
    g_index = index;
    g_capacity_pixels = pixels;
    g_min_div = div;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.prepared = true;
    g_status.source_generation = info.generation;
    g_status.source_bytes = info.size;
    g_status.frame_count = count;
    g_status.source_width = src_w;
    g_status.source_height = src_h;
    g_status.width = out_w;
    g_status.height = out_h;
    g_status.min_scale_div = div;
    g_status.scale_div = div;
    g_status.pixel_bytes = pixels;
    g_status.transport_bytes = pixels + 4;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t prisma_video_process_render_frame(uint16_t index, uint8_t requested_div)
{
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool prepared = g_status.prepared && g_video && g_decode && g_index && index < g_frame_count;
    frame_ref_t ref = prepared ? g_frames[index] : (frame_ref_t){0};
    uint8_t min_div = g_min_div;
    xSemaphoreGive(g_lock);
    if (!prepared) return ESP_ERR_INVALID_ARG;

    uint8_t div = valid_div(requested_div) ? requested_div : min_div;
    if (div < min_div) div = min_div;

    int64_t started = esp_timer_get_time();
    const uint8_t *jpeg = g_video + ref.offset;
    uint16_t src_w = 0, src_h = 0;
    if (!baseline_size(jpeg, ref.length, &src_w, &src_h)) {
        set_error("frame is not baseline JPEG");
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t requested_pixels = (size_t)(src_w / div) * (src_h / div);
    if (!requested_pixels || requested_pixels > g_capacity_pixels) {
        set_error("requested adaptive VIDEO scale exceeds prepared buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg,
        .indata_size = ref.length,
        .outbuf = (uint8_t *)g_decode,
        .outbuf_size = g_capacity_pixels * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale_enum(div),
        .flags = { .swap_color_bytes = 0 },
    };
    esp_jpeg_image_output_t decoded = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &decoded);
    if (err != ESP_OK) {
        set_error("ESP JPEG decoder failed on VIDEO frame");
        return err;
    }

    size_t n = (size_t)decoded.width * decoded.height;
    if (n > g_capacity_pixels) {
        set_error("decoded VIDEO frame exceeded prepared buffer");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < n; ++i) g_index[i] = poster_index(g_decode[i]);

    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status.valid = true;
    g_status.error[0] = '\0';
    g_status.last_frame = index;
    g_status.width = decoded.width;
    g_status.height = decoded.height;
    g_status.scale_div = div;
    g_status.last_frame_ms = elapsed;
    g_status.pixel_bytes = n;
    g_status.transport_bytes = n + 4;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
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

esp_err_t prisma_video_process_send_indexed(httpd_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_status.valid || !g_index) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t w = g_status.width;
    uint16_t h = g_status.height;
    size_t n = (size_t)w * h;
    uint8_t header[4];
    le16(&header[0], w);
    le16(&header[2], h);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Prisma-Format", "POSTER64-INDEX8");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)g_index, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(g_lock);
    return err;
}

esp_err_t prisma_video_process_send_bmp(httpd_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_video_process_init() != ESP_OK) return ESP_ERR_NO_MEM;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_status.valid || !g_index) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NOT_FOUND;
    }

    const uint16_t w = g_status.width;
    const uint16_t h = g_status.height;
    const uint32_t palette_bytes = 64u * 4u;
    const uint32_t pixel_offset = 54u + palette_bytes;
    const uint32_t row_bytes = ((uint32_t)w + 3u) & ~3u;
    const uint32_t image_bytes = row_bytes * h;
    const uint32_t file_bytes = pixel_offset + image_bytes;

    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    le32(&header[2], file_bytes);
    le32(&header[10], pixel_offset);
    le32(&header[14], 40);
    le32(&header[18], w);
    le32(&header[22], h);
    le16(&header[26], 1);
    le16(&header[28], 8);
    le32(&header[34], image_bytes);
    le32(&header[46], 64);
    le32(&header[50], 64);

    static const uint8_t rb[4] = {0, 82, 173, 255};
    static const uint8_t gg[4] = {0, 85, 170, 255};
    uint8_t palette[64 * 4];
    for (int i = 0; i < 64; ++i) {
        uint8_t rl = (uint8_t)((i >> 4) & 3);
        uint8_t gl = (uint8_t)((i >> 2) & 3);
        uint8_t bl = (uint8_t)(i & 3);
        palette[i * 4 + 0] = rb[bl];
        palette[i * 4 + 1] = gg[gl];
        palette[i * 4 + 2] = rb[rl];
        palette[i * 4 + 3] = 0;
    }

    uint8_t *row = malloc(row_bytes);
    if (!row) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)palette, sizeof(palette));
    for (int y = h - 1; err == ESP_OK && y >= 0; --y) {
        memset(row, 0, row_bytes);
        memcpy(row, g_index + (size_t)y * w, w);
        err = httpd_resp_send_chunk(req, (const char *)row, row_bytes);
    }
    free(row);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(g_lock);
    return err;
}
