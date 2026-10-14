#include "appliance_results.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "local_process.h"
#include "processed_store.h"
#include "stream_control.h"

static char s_last_generator[16];

static inline uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                            ((uint16_t)(g & 0xFC) << 3) |
                            (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static inline uint16_t poster64_to_lcd(uint8_t z)
{
    static const uint8_t rb[4] = {0, 82, 173, 255};
    static const uint8_t gg[4] = {0, 85, 170, 255};
    uint8_t r = (z >> 4) & 3u;
    uint8_t g = (z >> 2) & 3u;
    uint8_t b = z & 3u;
    return lcd_rgb(rb[r], gg[g], rb[b]);
}

static void fill(uint16_t *dst, uint16_t w, uint16_t h, uint16_t color)
{
    if (!dst) return;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) dst[i] = color;
}

static void fill_rect(uint16_t *dst, uint16_t w, uint16_t h,
                      int x, int y, int rw, int rh, uint16_t color)
{
    if (!dst) return;
    for (int yy = y; yy < y + rh; ++yy) {
        if (yy < 0 || yy >= h) continue;
        for (int xx = x; xx < x + rw; ++xx) {
            if (xx < 0 || xx >= w) continue;
            dst[(size_t)yy * w + xx] = color;
        }
    }
}

void prisma_appliance_overlay_dots(uint16_t *dst,
                                   uint16_t dst_width,
                                   uint16_t dst_height,
                                   uint8_t page_index,
                                   uint8_t page_count)
{
    if (!dst || !dst_width || dst_height < 8 || !page_count) return;
    const int size = 5;
    const int gap = 7;
    const int total = page_count * size + (page_count - 1) * gap;
    const int x0 = ((int)dst_width - total) / 2;
    const int y = (int)dst_height - 13;
    uint16_t active = lcd_rgb(35, 220, 220);
    uint16_t dim = lcd_rgb(45, 60, 75);
    for (uint8_t i = 0; i < page_count; ++i)
        fill_rect(dst, dst_width, dst_height,
                  x0 + i * (size + gap), y, size, size,
                  i == page_index ? active : dim);
}

esp_err_t prisma_appliance_render_image_result(uint16_t *dst,
                                               uint16_t dst_width,
                                               uint16_t dst_height,
                                               uint16_t *source_width,
                                               uint16_t *source_height)
{
    if (!dst || !dst_width || !dst_height) return ESP_ERR_INVALID_ARG;
    if (source_width) *source_width = 0;
    if (source_height) *source_height = 0;

    if (!prisma_appliance_stream_enabled()) {
        fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
        prisma_appliance_overlay_dots(dst, dst_width, dst_height, 4, 7);
        return ESP_OK;
    }

    prisma_processed_info_t info = {0};
    esp_err_t err = prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &info);
    if (err != ESP_OK || !info.valid || info.format != PRISMA_PROCESSED_FMT_INDEX8 ||
        !info.width || !info.height) {
        fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
        prisma_appliance_overlay_dots(dst, dst_width, dst_height, 4, 7);
        return ESP_OK;
    }

    if (source_width) *source_width = info.width;
    if (source_height) *source_height = info.height;

    fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));

    uint32_t fit_w = dst_width;
    uint32_t fit_h = ((uint32_t)info.height * dst_width) / info.width;
    if (fit_h > dst_height) {
        fit_h = dst_height;
        fit_w = ((uint32_t)info.width * dst_height) / info.height;
    }
    if (!fit_w) fit_w = 1;
    if (!fit_h) fit_h = 1;

    uint16_t x0 = (uint16_t)((dst_width - fit_w) / 2u);
    uint16_t y0 = (uint16_t)((dst_height - fit_h) / 2u);

    uint8_t *row = heap_caps_malloc(info.width, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) row = malloc(info.width);
    if (!row) {
        fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
        prisma_appliance_overlay_dots(dst, dst_width, dst_height, 4, 7);
        return ESP_OK;
    }

    uint16_t last_sy = UINT16_MAX;
    for (uint16_t y = 0; y < fit_h; ++y) {
        uint16_t sy = (uint16_t)(((uint32_t)y * info.height) / fit_h);
        if (sy >= info.height) sy = info.height - 1;
        if (sy != last_sy) {
            err = prisma_processed_read(PRISMA_PROCESSED_IMAGE_OUT,
                                        (size_t)sy * info.width,
                                        row,
                                        info.width);
            if (err != ESP_OK) break;
            last_sy = sy;
        }
        uint16_t *d = dst + (size_t)(y0 + y) * dst_width + x0;
        for (uint16_t x = 0; x < fit_w; ++x) {
            uint16_t sx = (uint16_t)(((uint32_t)x * info.width) / fit_w);
            if (sx >= info.width) sx = info.width - 1;
            d[x] = poster64_to_lcd(row[sx]);
        }
    }

    free(row);
    if (err != ESP_OK) fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
    prisma_appliance_overlay_dots(dst, dst_width, dst_height, 4, 7);
    return ESP_OK;
}

static void plot(uint16_t *dst, int w, int h, int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
        dst[(size_t)y * w + x] = c;
}

static void draw_line(uint16_t *dst, int w, int h,
                      int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        plot(dst, w, h, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void draw_rect(uint16_t *dst, int w, int h,
                      int x, int y, int rw, int rh, uint16_t c)
{
    draw_line(dst, w, h, x, y, x + rw, y, c);
    draw_line(dst, w, h, x + rw, y, x + rw, y + rh, c);
    draw_line(dst, w, h, x + rw, y + rh, x, y + rh, c);
    draw_line(dst, w, h, x, y + rh, x, y, c);
}

static void plot8(uint16_t *dst, int w, int h,
                  int cx, int cy, int x, int y, uint16_t c)
{
    int a[8][2] = {
        {cx + x, cy + y}, {cx - x, cy + y}, {cx + x, cy - y}, {cx - x, cy - y},
        {cx + y, cy + x}, {cx - y, cy + x}, {cx + y, cy - x}, {cx - y, cy - x}
    };
    for (int i = 0; i < 8; ++i) plot(dst, w, h, a[i][0], a[i][1], c);
}

static void draw_circle(uint16_t *dst, int w, int h,
                        int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, e = 1 - r;
    while (x >= y) {
        plot8(dst, w, h, cx, cy, x, y, c);
        y++;
        if (e < 0) e += 2 * y + 1;
        else { x--; e += 2 * (y - x) + 1; }
    }
}

static bool is_generator_name(const char *name)
{
    return name && (!strcmp(name, "lines") || !strcmp(name, "rectangles") ||
                    !strcmp(name, "circles") || !strcmp(name, "triangles"));
}

static void remember_generator(void)
{
    prisma_process_status_t st = {0};
    if (prisma_process_get_status(&st) == ESP_OK && st.valid && is_generator_name(st.last_test))
        strlcpy(s_last_generator, st.last_test, sizeof(s_last_generator));
}

esp_err_t prisma_appliance_render_generator_result(uint16_t *dst,
                                                   uint16_t dst_width,
                                                   uint16_t dst_height,
                                                   char *generator_name,
                                                   size_t generator_name_size)
{
    if (!dst || !dst_width || !dst_height) return ESP_ERR_INVALID_ARG;
    if (generator_name && generator_name_size) generator_name[0] = '\0';

    if (!prisma_appliance_stream_enabled()) {
        fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
        prisma_appliance_overlay_dots(dst, dst_width, dst_height, 5, 7);
        return ESP_OK;
    }

    remember_generator();
    if (!s_last_generator[0]) {
        fill(dst, dst_width, dst_height, lcd_rgb(4, 7, 12));
        prisma_appliance_overlay_dots(dst, dst_width, dst_height, 5, 7);
        return ESP_OK;
    }

    if (generator_name && generator_name_size)
        strlcpy(generator_name, s_last_generator, generator_name_size);

    uint16_t bg = lcd_rgb(4, 7, 12);
    uint16_t a = lcd_rgb(40, 215, 215);
    uint16_t b = lcd_rgb(240, 245, 250);
    fill(dst, dst_width, dst_height, bg);

    int w = dst_width;
    int h = dst_height;
    int cx = w / 2;
    int cy = h / 2;

    if (!strcmp(s_last_generator, "lines")) {
        for (int i = 0; i < 18; ++i) {
            int y = 8 + i * 13;
            if (y >= h) break;
            draw_line(dst, w, h, 8, y, w - 9, h - 1 - y, (i & 1) ? a : b);
        }
        for (int i = 0; i < 8; ++i) {
            int x = 18 + i * 29;
            if (x >= w) break;
            draw_line(dst, w, h, x, 8, w - 1 - x, h - 9, a);
        }
    } else if (!strcmp(s_last_generator, "rectangles")) {
        for (int i = 0; i < 9; ++i) {
            int z = 10 + i * 10;
            if (w - 1 - 2 * z <= 0 || h - 1 - 2 * z <= 0) break;
            draw_rect(dst, w, h, z, z, w - 1 - 2 * z, h - 1 - 2 * z,
                      (i & 1) ? a : b);
        }
    } else if (!strcmp(s_last_generator, "circles")) {
        int maxr = (w < h ? w : h) / 2 - 15;
        for (int r = 14, i = 0; r <= maxr; r += 13, ++i)
            draw_circle(dst, w, h, cx, cy, r, (i & 1) ? a : b);
    } else if (!strcmp(s_last_generator, "triangles")) {
        for (int i = 0; i < 9; ++i) {
            int z = 10 + i * 10;
            if (z >= cx || z >= cy) break;
            uint16_t c = (i & 1) ? a : b;
            draw_line(dst, w, h, cx, z, w - 1 - z, h - 1 - z, c);
            draw_line(dst, w, h, w - 1 - z, h - 1 - z, z, h - 1 - z, c);
            draw_line(dst, w, h, z, h - 1 - z, cx, z, c);
        }
    }

    prisma_appliance_overlay_dots(dst, dst_width, dst_height, 5, 7);
    return ESP_OK;
}
