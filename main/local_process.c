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

#define PROCESS_TARGET_LONG_SIDE 800u
#define PROCESS_PSRAM_RESERVE (2u * 1024u * 1024u)
#define TEST_W 240
#define TEST_H 240

static const char *TAG = "PRISMA_PROCESS";

static SemaphoreHandle_t g_lock;
static uint16_t *g_pixels;
static bool g_poster_palette;
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
    g_status.valid = false;
    g_status.width = 0;
    g_status.height = 0;
    g_status.pixel_bytes = 0;
    if (test) strlcpy(g_status.last_test, test, sizeof(g_status.last_test));
    strlcpy(g_status.error, message ? message : "processing failed", sizeof(g_status.error));
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
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
    g_poster_palette = strcmp(name, "poster") == 0;
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
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram > PROCESS_PSRAM_RESERVE ? free_psram - PROCESS_PSRAM_RESERVE : 0;
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint8_t div = divs[i];
        uint32_t w = width / div;
        uint32_t h = height / div;
        size_t bytes = (size_t)w * h * sizeof(uint16_t);
        uint32_t long_side = w > h ? w : h;
        if (w && h && long_side <= PROCESS_TARGET_LONG_SIDE && bytes <= budget) return div;
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
        if (marker == 0xC2) return false;
        p += seglen;
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

static inline uint16_t posterize_rgb565(uint16_t c)
{
    static const uint16_t r5[4] = {0, 10, 21, 31};
    static const uint16_t g6[4] = {0, 21, 42, 63};
    uint8_t rl = level5((c >> 11) & 0x1F);
    uint8_t gl = level6((c >> 5) & 0x3F);
    uint8_t bl = level5(c & 0x1F);
    return (uint16_t)((r5[rl] << 11) | (g6[gl] << 5) | r5[bl]);
}

static inline uint8_t poster_index(uint16_t c)
{
    uint8_t rl = level5((c >> 11) & 0x1F);
    uint8_t gl = level6((c >> 5) & 0x3F);
    uint8_t bl = level5(c & 0x1F);
    return (uint8_t)((rl << 4) | (gl << 2) | bl);
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
        status_error("poster", "decoded IMAGE exceeds adaptive PSRAM budget");
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
        .flags = {.swap_color_bytes = 0},
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
    ESP_LOGI(TAG, "POSTER adaptive source_gen=%lu %ux%u div=%u %lums",
             (unsigned long)info.generation, decoded.width, decoded.height, div, (unsigned long)elapsed);
    return ESP_OK;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static uint16_t *new_test_canvas(const char *name)
{
    size_t bytes = TEST_W * TEST_H * sizeof(uint16_t);
    uint16_t *pixels = psram_alloc(bytes);
    if (!pixels) {
        status_error(name, "not enough RAM/PSRAM for primitive buffer");
        return NULL;
    }
    uint16_t bg = rgb565(4, 7, 12);
    for (size_t i = 0; i < TEST_W * TEST_H; ++i) pixels[i] = bg;
    return pixels;
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

static void draw_rect(uint16_t *px, int x, int y, int w, int h, uint16_t c)
{
    draw_line(px, TEST_W, TEST_H, x, y, x + w, y, c);
    draw_line(px, TEST_W, TEST_H, x + w, y, x + w, y + h, c);
    draw_line(px, TEST_W, TEST_H, x + w, y + h, x, y + h, c);
    draw_line(px, TEST_W, TEST_H, x, y + h, x, y, c);
}

static void plot8(uint16_t *px, int cx, int cy, int x, int y, uint16_t c)
{
    int pts[8][2] = {{cx+x,cy+y},{cx-x,cy+y},{cx+x,cy-y},{cx-x,cy-y},{cx+y,cy+x},{cx-y,cy+x},{cx+y,cy-x},{cx-y,cy-x}};
    for (int i=0;i<8;i++) if ((unsigned)pts[i][0] < TEST_W && (unsigned)pts[i][1] < TEST_H) px[pts[i][1]*TEST_W+pts[i][0]]=c;
}

static void draw_circle(uint16_t *px, int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot8(px, cx, cy, x, y, c);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y-x) + 1; }
    }
}

static void finish_generated(const char *name, uint16_t *pixels, int64_t started)
{
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    commit_result(name, pixels, TEST_W, TEST_H, elapsed, 0, 1);
    ESP_LOGI(TAG, "%s local %dx%d %lums", name, TEST_W, TEST_H, (unsigned long)elapsed);
}

esp_err_t prisma_process_run_lines(void)
{
    if (!begin_test("lines")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();
    uint16_t *pixels = new_test_canvas("lines");
    if (!pixels) return ESP_ERR_NO_MEM;
    uint16_t c1 = rgb565(40, 215, 215), c2 = rgb565(240, 245, 250);
    for (int i = 0; i < 18; ++i) {
        int y = 8 + i * 13;
        draw_line(pixels, TEST_W, TEST_H, 8, y, TEST_W - 9, TEST_H - 1 - y, (i & 1) ? c1 : c2);
    }
    for (int i = 0; i < 8; ++i) {
        int x = 18 + i * 29;
        draw_line(pixels, TEST_W, TEST_H, x, 8, TEST_W - 1 - x, TEST_H - 9, c1);
    }
    finish_generated("lines", pixels, started);
    return ESP_OK;
}

esp_err_t prisma_process_run_rectangles(void)
{
    if (!begin_test("rectangles")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();
    uint16_t *pixels = new_test_canvas("rectangles");
    if (!pixels) return ESP_ERR_NO_MEM;
    uint16_t c1 = rgb565(40, 215, 215), c2 = rgb565(240, 245, 250);
    for (int i=0;i<9;i++) {
        int inset = 10 + i*10;
        draw_rect(pixels, inset, inset, TEST_W-1-2*inset, TEST_H-1-2*inset, (i&1)?c1:c2);
    }
    finish_generated("rectangles", pixels, started);
    return ESP_OK;
}

esp_err_t prisma_process_run_circles(void)
{
    if (!begin_test("circles")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();
    uint16_t *pixels = new_test_canvas("circles");
    if (!pixels) return ESP_ERR_NO_MEM;
    uint16_t c1 = rgb565(40, 215, 215), c2 = rgb565(240, 245, 250);
    for (int r=14, i=0; r<=105; r+=13, i++) draw_circle(pixels, 120, 120, r, (i&1)?c1:c2);
    finish_generated("circles", pixels, started);
    return ESP_OK;
}

esp_err_t prisma_process_run_triangles(void)
{
    if (!begin_test("triangles")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();
    uint16_t *pixels = new_test_canvas("triangles");
    if (!pixels) return ESP_ERR_NO_MEM;
    uint16_t c1 = rgb565(40, 215, 215), c2 = rgb565(240, 245, 250);
    for (int i=0;i<9;i++) {
        int inset = 10 + i*10;
        int x1 = 120, y1 = inset;
        int x2 = TEST_W-1-inset, y2 = TEST_H-1-inset;
        int x3 = inset, y3 = TEST_H-1-inset;
        uint16_t c = (i&1)?c1:c2;
        draw_line(pixels, TEST_W, TEST_H, x1,y1,x2,y2,c);
        draw_line(pixels, TEST_W, TEST_H, x2,y2,x3,y3,c);
        draw_line(pixels, TEST_W, TEST_H, x3,y3,x1,y1,c);
    }
    finish_generated("triangles", pixels, started);
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

static esp_err_t send_poster8_bmp(httpd_req_t *req, uint16_t w, uint16_t h)
{
    const uint32_t palette_bytes = 64u * 4u;
    const uint32_t pixel_offset = 54u + palette_bytes;
    const uint32_t row_bytes = ((uint32_t)w + 3u) & ~3u;
    const uint32_t image_bytes = row_bytes * h;
    const uint32_t file_bytes = pixel_offset + image_bytes;
    uint8_t header[54] = {0};
    header[0]='B'; header[1]='M';
    le32(&header[2],file_bytes); le32(&header[10],pixel_offset); le32(&header[14],40);
    le32(&header[18],w); le32(&header[22],h); le16(&header[26],1); le16(&header[28],8);
    le32(&header[34],image_bytes); le32(&header[46],64); le32(&header[50],64);

    static const uint8_t rb[4]={0,82,173,255};
    static const uint8_t gg[4]={0,85,170,255};
    uint8_t palette[64*4];
    for(int i=0;i<64;i++){
        uint8_t rl=(i>>4)&3, gl=(i>>2)&3, bl=i&3;
        palette[i*4+0]=rb[bl]; palette[i*4+1]=gg[gl]; palette[i*4+2]=rb[rl]; palette[i*4+3]=0;
    }
    uint8_t *row=malloc(row_bytes);
    if(!row) return ESP_ERR_NO_MEM;
    esp_err_t err=httpd_resp_send_chunk(req,(const char*)header,sizeof(header));
    if(err==ESP_OK) err=httpd_resp_send_chunk(req,(const char*)palette,sizeof(palette));
    for(int y=h-1;err==ESP_OK && y>=0;--y){
        memset(row,0,row_bytes);
        for(uint16_t x=0;x<w;x++) row[x]=poster_index(g_pixels[(size_t)y*w+x]);
        err=httpd_resp_send_chunk(req,(const char*)row,row_bytes);
    }
    free(row);
    return err;
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
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=prisma-process.bmp");

    esp_err_t err;
    if (g_poster_palette) {
        err = send_poster8_bmp(req, w, h);
    } else {
        const uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
        const uint32_t image_bytes = row_bytes * h;
        const uint32_t file_bytes = 54u + image_bytes;
        uint8_t header[54] = {0};
        header[0] = 'B'; header[1] = 'M';
        le32(&header[2], file_bytes); le32(&header[10], 54); le32(&header[14], 40);
        le32(&header[18], w); le32(&header[22], h); le16(&header[26], 1); le16(&header[28], 24);
        le32(&header[34], image_bytes);
        uint8_t *row = malloc(row_bytes);
        if (!row) { xSemaphoreGive(g_lock); return ESP_ERR_NO_MEM; }
        err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
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
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(g_lock);
    return err;
}
