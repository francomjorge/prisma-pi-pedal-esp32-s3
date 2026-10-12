#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "wifi_portal.h"
#include "prisma_touch.h"
#include "storage.h"

#define LCD_HOST SPI2_HOST
#define W 240
#define H 240
#define LCD_PIN_SCLK 38
#define LCD_PIN_MOSI 39
#define LCD_PIN_CS 21
#define LCD_PIN_DC 45
#define LCD_PIN_RST 40
#define LCD_PIN_BL 46
#define BAT_POWER_PIN 2

#define HOME_COOLDOWN_TICKS 25
#define CONTROL_DEBOUNCE_TICKS 10
#define PAGE_COUNT 4
#define EFFECT_COUNT 6

#define FX_LABEL_X 10
#define FX_BAR_X 92
#define FX_BAR_W 102
#define FX_VALUE_X 205
#define FX_ROW_Y 20
#define FX_ROW_STEP 32
#define FX_TEXT_SCALE 2

static const char *TAG = "PRISMA";
static uint16_t frame[W * H];
static esp_lcd_panel_handle_t panel;
static int mode_index = 0;
static int intensity = 61;
static int glitch_count = 0;
static bool synth_on = true;
static bool effect_enabled[EFFECT_COUNT] = {false, false, false, false, false, true};
static int effect_value[EFFECT_COUNT] = {50, 50, 50, 50, 50, 50};
static const char *effect_names[EFFECT_COUNT] = {"PIXEL", "GLITCH", "POSTER", "HUE", "MIRROR", "MIX"};

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h && yy < H; yy++) {
        for (int xx = x; xx < x + w && xx < W; xx++) {
            if (xx >= 0 && yy >= 0) frame[yy * W + xx] = c;
        }
    }
}

static const uint8_t SP[5]={0},DA[5]={8,8,8,8,8},DT[5]={0,0x60,0x60,0,0},D0[5]={0x3e,0x51,0x49,0x45,0x3e},D1[5]={0,0x42,0x7f,0x40,0},D2[5]={0x42,0x61,0x51,0x49,0x46},D3[5]={0x21,0x41,0x45,0x4b,0x31},D4[5]={0x18,0x14,0x12,0x7f,0x10},D5[5]={0x27,0x45,0x45,0x45,0x39},D6[5]={0x3c,0x4a,0x49,0x49,0x30},D7[5]={1,0x71,9,5,3},D8[5]={0x36,0x49,0x49,0x49,0x36},D9[5]={6,0x49,0x49,0x29,0x1e};
static const uint8_t A[5]={0x7e,0x11,0x11,0x11,0x7e},B[5]={0x7f,0x49,0x49,0x49,0x36},C[5]={0x3e,0x41,0x41,0x41,0x22},D[5]={0x7f,0x41,0x41,0x22,0x1c},E[5]={0x7f,0x49,0x49,0x49,0x41},F[5]={0x7f,9,9,9,1},G[5]={0x3e,0x41,0x49,0x49,0x7a},Hh[5]={0x7f,8,8,8,0x7f},I[5]={0,0x41,0x7f,0x41,0},J[5]={0x20,0x40,0x41,0x3f,0x01},K[5]={0x7f,8,0x14,0x22,0x41},L[5]={0x7f,0x40,0x40,0x40,0x40},M[5]={0x7f,2,0x0c,2,0x7f},N[5]={0x7f,4,8,0x10,0x7f},O[5]={0x3e,0x41,0x41,0x41,0x3e},P[5]={0x7f,0x09,0x09,0x09,0x06},R[5]={0x7f,0x09,0x19,0x29,0x46},S[5]={0x46,0x49,0x49,0x49,0x31},T[5]={1,1,0x7f,1,1},U[5]={0x3f,0x40,0x40,0x40,0x3f},V[5]={0x1f,0x20,0x40,0x20,0x1f},Ww[5]={0x3f,0x40,0x38,0x40,0x3f},X[5]={0x63,0x14,0x08,0x14,0x63},Y[5]={7,8,0x70,8,7},Z[5]={0x61,0x51,0x49,0x45,0x43};

static const uint8_t *g(char c)
{
    if (c >= '0' && c <= '9') {
        static const uint8_t *q[] = {D0,D1,D2,D3,D4,D5,D6,D7,D8,D9};
        return q[c - '0'];
    }
    switch (c) {
        case '-': return DA; case '.': return DT;
        case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
        case 'E': return E; case 'F': return F; case 'G': return G; case 'H': return Hh;
        case 'I': return I; case 'J': return J; case 'K': return K; case 'L': return L;
        case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P;
        case 'R': return R; case 'S': return S; case 'T': return T; case 'U': return U;
        case 'V': return V; case 'W': return Ww; case 'X': return X; case 'Y': return Y;
        case 'Z': return Z; default: return SP;
    }
}

static void text(int x, int y, const char *s, int z, uint16_t c)
{
    while (*s) {
        const uint8_t *p = g(*s++);
        for (int a = 0; a < 5; a++) {
            for (int b = 0; b < 7; b++) {
                if (p[a] & (1 << b)) rect(x + a * z, y + b * z, z, z, c);
            }
        }
        x += 6 * z;
    }
}

static int text_width(const char *s, int z) { return (int)strlen(s) * 6 * z - z; }
static void text_center(int y, const char *s, int z, uint16_t c) { text((W - text_width(s, z)) / 2, y, s, z, c); }
static void text_box_center(int x, int w, int y, const char *s, int z, uint16_t c) { text(x + (w - text_width(s, z)) / 2, y, s, z, c); }
static void clear(uint16_t c) { for (int i = 0; i < W * H; i++) frame[i] = c; }
static void show(void) { ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, frame)); }

static void dots(int page, uint16_t cyan, uint16_t dim)
{
    int xs[4] = {92,108,124,140};
    for (int i = 0; i < PAGE_COUNT; i++) rect(xs[i], 224, 8, 8, i == page ? cyan : dim);
}

static void logo_title(int y, int phase)
{
    uint16_t wh = rgb(240,245,250);
    uint16_t pal[5] = {rgb(30,120,255),rgb(175,55,255),rgb(255,35,150),rgb(255,120,25),rgb(45,225,120)};
    int z = 4;
    const char *base = "PRISMA";
    const char *fx = "-PI";
    int total = text_width("PRISMA-PI", z);
    int x = (W - total) / 2;
    text(x, y, base, z, wh);
    int fx_x = x + 6 * z * 6;
    for (int i = 0; fx[i]; i++) {
        char one[2] = {fx[i], 0};
        text(fx_x + i * 6 * z, y, one, z, pal[(i + phase) % 5]);
    }
}

static void page_intro(int phase)
{
    uint16_t bg=rgb(4,7,12), wh=rgb(240,245,250), cy=rgb(35,220,220), dim=rgb(45,60,75);
    clear(bg);logo_title(42, phase);text_center(100, "LITE", 3, wh);text_center(151, "VIDEO SYNTHESIZER", 2, wh);dots(0, cy, dim);show();
}

static void glitch(uint16_t x, uint16_t y)
{
    uint16_t pal[5] = {rgb(30,120,255),rgb(175,55,255),rgb(255,35,150),rgb(255,120,25),rgb(45,225,120)};
    for (int i = 0; i < 9; i++) {int dx = ((i * 17 + glitch_count * 11) % 55) - 27;int dy = ((i * 23 + glitch_count * 7) % 55) - 27;int sz = 4 + ((i + glitch_count) % 4) * 3;rect((int)x + dx, (int)y + dy, sz, sz, pal[(i + glitch_count) % 5]);}
    glitch_count++;show();
}

static void page_controls(void)
{
    uint16_t bg=rgb(4,7,12), wh=rgb(240,245,250), cy=rgb(35,220,220), dim=rgb(45,60,75), disabled=rgb(75,82,90), blue=rgb(25,65,145), green=rgb(35,200,110), red=rgb(230,55,65);
    uint16_t fg = synth_on ? wh : disabled;uint16_t accent = synth_on ? cy : disabled;clear(bg);
    text(12,10,"PRISMA-PI",2,wh);rect(174,7,54,28,synth_on?green:red);text_box_center(174,54,14,synth_on?"ON":"OFF",2,wh);text(12,47,"NOW PLAYING",1,accent);text(12,60,prisma_storage_current(),2,fg);text(12,91,"INTENSITY",2,fg);
    char pct[8];snprintf(pct,sizeof(pct),"%d",intensity);text(198,91,pct,2,fg);rect(12,114,216,10,dim);rect(12,114,synth_on?(216*intensity/100):0,10,accent);text(12,151,"MODE",2,fg);rect(86,143,142,34,synth_on?blue:dim);text_box_center(86,142,154,mode_index?"BYPASS":"RANDOM",2,fg);dots(1,synth_on?cy:disabled,dim);show();
}

static void page_monitor(void)
{
    uint16_t bg=rgb(4,7,12), wh=rgb(240,245,250), cy=rgb(35,220,220), dim=rgb(45,60,75);clear(bg);text_center(48,"VISUAL MONITOR",2,wh);rect(28,82,184,2,cy);text_center(106,"OPEN IN BROWSER",2,wh);text_center(143,"192.168.4.1",2,cy);dots(2,cy,dim);show();
}

static void page_effects(void)
{
    uint16_t bg=rgb(4,7,12), wh=rgb(240,245,250), cy=rgb(35,220,220), dim=rgb(45,60,75), disabled=rgb(75,82,90), green=rgb(35,200,110), red=rgb(230,55,65);clear(bg);
    for (int i = 0; i < EFFECT_COUNT; i++) {
        int y = FX_ROW_Y + i * FX_ROW_STEP;bool enabled = (i == 5) ? true : effect_enabled[i];uint16_t label = !synth_on ? disabled : (enabled ? wh : dim);text(FX_LABEL_X, y, effect_names[i], FX_TEXT_SCALE, label);
        if (i == 4) {int sy = y - 2;rect(FX_BAR_X, sy, 48, 20, enabled ? (synth_on ? green : disabled) : dim);text_box_center(FX_BAR_X,48,sy+7,"ON",1,wh);rect(FX_BAR_X+54,sy,48,20,!enabled ? (synth_on ? red : disabled) : dim);text_box_center(FX_BAR_X+54,48,sy+7,"OFF",1,wh);} else {char value[4];snprintf(value,sizeof(value),"%d",effect_value[i]);rect(FX_BAR_X,y+3,FX_BAR_W,8,dim);int fill = FX_BAR_W * effect_value[i] / 100;if (fill > 0) rect(FX_BAR_X,y+3,fill,8,(synth_on&&enabled)?cy:disabled);text(FX_VALUE_X,y+3,value,1,label);}
    }
    dots(3,synth_on?cy:disabled,dim);show();
}

static void power_hold(void){gpio_config_t p = {.pin_bit_mask=1ULL<<BAT_POWER_PIN,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&p));ESP_ERROR_CHECK(gpio_set_level(BAT_POWER_PIN,1));}
static void draw_page(int page, int phase){if (page == 0) page_intro(phase);else if (page == 1) page_controls();else if (page == 2) page_monitor();else page_effects();}
static void toggle_power(void){bool was = synth_on;synth_on = !synth_on;if (!was && synth_on) mode_index = 1;ESP_LOGI(TAG,"SYNTH -> %s",synth_on?"ON":"OFF");}
static bool page_needs_input_debounce(int page){return page == 1 || page == 3;}

void app_main(void)
{
    power_hold();ESP_LOGI(TAG,"PRISMA-PI LITE M4.2I PAGE2 DEBOUNCE + COMPACT PAGE4");
    gpio_config_t bl = {.pin_bit_mask=1ULL<<LCD_PIN_BL,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&bl));gpio_set_level(LCD_PIN_BL,0);
    spi_bus_config_t bus = {.sclk_io_num=LCD_PIN_SCLK,.mosi_io_num=LCD_PIN_MOSI,.miso_io_num=-1,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=W*40*2};ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io = NULL;esp_lcd_panel_io_spi_config_t ioc = {.dc_gpio_num=LCD_PIN_DC,.cs_gpio_num=LCD_PIN_CS,.pclk_hz=40000000,.lcd_cmd_bits=8,.lcd_param_bits=8,.spi_mode=0,.trans_queue_depth=10};ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,&ioc,&io));
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num=LCD_PIN_RST,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&pc,&panel));ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
    int phase = 0;page_intro(phase);gpio_set_level(LCD_PIN_BL,1);
    bool touch = prisma_touch_init();ESP_LOGI(TAG,"TOUCH: %s",touch?"READY":"FAILED");prisma_wifi_start();for (int i=0;i<EFFECT_COUNT;i++) prisma_control_set_effect(i,effect_enabled[i]);
    int page = 0;uint16_t x=0,y=0,startx=0,starty=0;bool down=false;bool input_lock=false;int input_lock_ticks=0;int miss=0,anim=0,home_cooldown=0;
    while (1) {
        if (home_cooldown > 0) home_cooldown--;bool touched = touch && prisma_touch_read(&x,&y);
        if (input_lock) {down=false;miss=0;if (touched) input_lock_ticks=CONTROL_DEBOUNCE_TICKS;else if (input_lock_ticks>0) input_lock_ticks--;else input_lock=false;}
        else if (touched) {miss=0;if (!down) {startx=x;starty=y;down=true;} else if ((int)startx-(int)x>55 && page<PAGE_COUNT-1) {page++;glitch_count=0;draw_page(page,phase);down=false;if (page_needs_input_debounce(page)) {input_lock=true;input_lock_ticks=CONTROL_DEBOUNCE_TICKS;}} else if ((int)x-(int)startx>55 && page>0) {page--;glitch_count=0;if (page==0) home_cooldown=HOME_COOLDOWN_TICKS;draw_page(page,phase);down=false;if (page_needs_input_debounce(page)) {input_lock=true;input_lock_ticks=CONTROL_DEBOUNCE_TICKS;}}}
        else if (down && ++miss>4) {
            if (page==0 && home_cooldown==0) glitch(startx,starty);
            else if (page==1 && startx>=170 && startx<=232 && starty>=5 && starty<=42) {toggle_power();page_controls();}
            else if (page==1 && startx>=12 && startx<=165 && starty>=45 && starty<=85) {const char *src=prisma_storage_next();prisma_control_set_filename(src);page_controls();ESP_LOGI(TAG,"SOURCE -> %s",src);}
            else if (page==1 && synth_on && startx>=12 && startx<=228 && starty>=106 && starty<=132) {intensity=((int)startx-12)*100/216;if (intensity<0) intensity=0;if (intensity>100) intensity=100;page_controls();ESP_LOGI(TAG,"INTENSITY -> %d",intensity);}
            else if (page==1 && synth_on && startx>=82 && startx<=232 && starty>=139 && starty<=181) {mode_index=mode_index?0:1;page_controls();ESP_LOGI(TAG,"MODE -> %s",mode_index?"BYPASS":"RANDOM");}
            else if (page==3 && starty>=FX_ROW_Y-6 && starty<FX_ROW_Y-6+EFFECT_COUNT*FX_ROW_STEP) {
                int effect=((int)starty-(FX_ROW_Y-6))/FX_ROW_STEP;
                if (effect>=0 && effect<EFFECT_COUNT) {
                    if (effect==4) {if (startx>=FX_BAR_X && startx<FX_BAR_X+51) {effect_enabled[4]=true;prisma_control_set_effect(4,true);ESP_LOGI(TAG,"EFFECT MIRROR -> ON");} else if (startx>=FX_BAR_X+51 && startx<=FX_BAR_X+FX_BAR_W) {effect_enabled[4]=false;prisma_control_set_effect(4,false);ESP_LOGI(TAG,"EFFECT MIRROR -> OFF");}}
                    else if (startx<FX_BAR_X) {if (effect<EFFECT_COUNT-1) {effect_enabled[effect]=!effect_enabled[effect];prisma_control_set_effect(effect,effect_enabled[effect]);ESP_LOGI(TAG,"EFFECT %s -> %s",effect_names[effect],effect_enabled[effect]?"ON":"OFF");} else {effect_enabled[5]=true;prisma_control_set_effect(5,true);}}
                    else if (synth_on && startx<=FX_BAR_X+FX_BAR_W) {int value=((int)startx-FX_BAR_X)*100/FX_BAR_W;if (value<0) value=0;if (value>100) value=100;effect_value[effect]=value;ESP_LOGI(TAG,"EFFECT %s LEVEL -> %d",effect_names[effect],value);}
                    page_effects();
                }
            }
            down=false;miss=0;
        }
        if (page==0 && glitch_count==0 && ++anim>=6) {anim=0;phase=(phase+1)%5;page_intro(phase);}vTaskDelay(pdMS_TO_TICKS(20));
    }
}
