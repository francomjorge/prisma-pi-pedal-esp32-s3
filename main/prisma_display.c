#include "prisma_display.h"
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#define LCD_HOST SPI2_HOST
#define LCD_W 240
#define LCD_H 240
#define LCD_PIN_SCLK 38
#define LCD_PIN_MOSI 39
#define LCD_PIN_CS 21
#define LCD_PIN_DC 45
#define LCD_PIN_RST 40
#define LCD_PIN_BL 46

static const char *TAG = "PEDAL_DISPLAY";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

static void fill(uint16_t c)
{
    for (int i = 0; i < LCD_W * LCD_H; ++i) s_frame[i] = c;
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h && yy < LCD_H; ++yy) {
        for (int xx = x; xx < x + w && xx < LCD_W; ++xx) {
            if (xx >= 0 && yy >= 0) s_frame[yy * LCD_W + xx] = c;
        }
    }
}

static void show(void)
{
    if (s_panel && s_frame) {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_frame));
    }
}

bool prisma_display_init(void)
{
    s_frame = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!s_frame) {
        ESP_LOGE(TAG, "frame allocation failed");
        return false;
    }

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&bl) != ESP_OK) return false;
    gpio_set_level(LCD_PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) return false;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) return false;
    if (esp_lcd_panel_reset(s_panel) != ESP_OK) return false;
    if (esp_lcd_panel_init(s_panel) != ESP_OK) return false;
    if (esp_lcd_panel_invert_color(s_panel, true) != ESP_OK) return false;
    if (esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) return false;

    fill(rgb565(4, 7, 12));
    rect(0, 0, LCD_W, 8, rgb565(35, 220, 220));
    show();
    gpio_set_level(LCD_PIN_BL, 1);
    ESP_LOGI(TAG, "ST7789 ready 240x240");
    return true;
}

void prisma_display_boot_stage(unsigned stage, bool ok)
{
    if (!s_frame) return;
    const uint16_t bg = rgb565(4, 7, 12);
    const uint16_t good = rgb565(35, 200, 110);
    const uint16_t bad = rgb565(230, 55, 65);
    const uint16_t cyan = rgb565(35, 220, 220);
    fill(bg);
    rect(0, 0, LCD_W, 8, cyan);
    for (unsigned i = 0; i <= stage && i < 5; ++i) {
        rect(20 + (int)i * 42, 102, 28, 28, i == stage ? (ok ? good : bad) : good);
    }
    show();
}

void prisma_display_touch(uint16_t x, uint16_t y)
{
    if (!s_frame) return;
    fill(rgb565(4, 7, 12));
    rect(0, 0, LCD_W, 8, rgb565(35, 220, 220));
    rect((int)x - 4, (int)y - 4, 9, 9, rgb565(255, 35, 150));
    show();
}

void prisma_display_swipe(bool left)
{
    if (!s_frame) return;
    fill(left ? rgb565(20, 70, 45) : rgb565(55, 25, 85));
    rect(0, 0, LCD_W, 8, rgb565(35, 220, 220));
    show();
}
