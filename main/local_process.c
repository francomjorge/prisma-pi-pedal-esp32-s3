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
#include "poster_config.h"
#include "processed_store.h"

#define PROCESS_PSRAM_RESERVE (1536u * 1024u)
#define TEST_W 240
#define TEST_H 240

static const char *TAG = "PRISMA_PROCESS";
static SemaphoreHandle_t g_lock;
static uint16_t *g_pixels;
static bool g_flash_result;
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
    if (test) strlcpy(g_status.last_test, test, sizeof(g_status.last_test));
    strlcpy(g_status.error, message ? message : "processing failed", sizeof(g_status.error));
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
}

static bool begin_test(const char *name)
{
    if (!g_lock && prisma_process_init() != ESP_OK) return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_status.busy) { xSemaphoreGive(g_lock); return false; }
    g_status.busy = true;
    g_status.error[0] = '\0';
    strlcpy(g_status.last_test, name, sizeof(g_status.last_test));
    xSemaphoreGive(g_lock);
    return true;
}

static void commit_flash_status(const char *name,
                                uint16_t sw, uint16_t sh,
                                uint16_t w, uint16_t h,
                                uint32_t elapsed, uint32_t generation,
                                uint8_t div, size_t payload)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    free(g_pixels);
    g_pixels = NULL;
    g_flash_result = true;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.valid = true;
    strlcpy(g_status.last_test, name, sizeof(g_status.last_test));
    g_status.source_width = sw;
    g_status.source_height = sh;
    g_status.width = w;
    g_status.height = h;
    g_status.elapsed_ms = elapsed;
    g_status.source_generation = generation;
    g_status.pixel_bytes = payload;
    g_status.jpeg_scale_div = div;
    g_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    xSemaphoreGive(g_lock);
}

static void commit_generator(const char *name, uint16_t *pixels, uint32_t elapsed)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    uint16_t *old = g_pixels;
    g_pixels = pixels;
    g_flash_result = false;
    memset(&g_status, 0, sizeof(g_status));
    g_status.ready = true;
    g_status.valid = true;
    strlcpy(g_status.last_test, name, sizeof(g_status.last_test));
    g_status.width = TEST_W;
    g_status.height = TEST_H;
    g_status.elapsed_ms = elapsed;
    g_status.pixel_bytes = TEST_W * TEST_H * sizeof(uint16_t);
    g_status.jpeg_scale_div = 1;
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

static inline uint8_t poster_index(uint16_t c)
{
    uint8_t rl = level5((c >> 11) & 0x1F);
    uint8_t gl = level6((c >> 5) & 0x3F);
    uint8_t bl = level5(c & 0x1F);
    return (uint8_t)((rl << 4) | (gl << 2) | bl);
}

static uint8_t choose_decode_div(uint16_t sw, uint16_t sh, uint16_t tw, uint16_t th, size_t budget)
{
    const uint8_t divs[] = {8,4,2,1};
    size_t out_bytes = (size_t)tw * th;
    uint16_t need_w = tw < sw ? tw : sw;
    uint16_t need_h = th < sh ? th : sh;
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint8_t d = divs[i];
        uint16_t w = sw / d, h = sh / d;
        if (!w || !h || w < need_w || h < need_h) continue;
        if ((size_t)w * h * sizeof(uint16_t) + out_bytes <= budget) return d;
    }
    for (size_t i = 0; i < sizeof(divs); ++i) {
        uint8_t d = divs[i];
        uint16_t w = sw / d, h = sh / d;
        if (!w || !h) continue;
        if ((size_t)w * h * sizeof(uint16_t) + out_bytes <= budget) return d;
    }
    return 0;
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
        *cw = w; *x0 = (uint16_t)((sw - w) / 2u);
    } else if (lhs < rhs) {
        uint16_t h = (uint16_t)(((uint32_t)sw * th) / tw);
        if (!h) h = 1;
        *ch = h; *y0 = (uint16_t)((sh - h) / 2u);
    }
}

static void rgb565_to_index(const uint16_t *src, uint16_t sw, uint16_t sh,
                            uint8_t *dst, uint16_t tw, uint16_t th)
{
    uint16_t x0,y0,cw,ch;
    crop_for_target(sw,sh,tw,th,&x0,&y0,&cw,&ch);
    for (uint16_t y = 0; y < th; ++y) {
        uint16_t sy = (uint16_t)(y0 + ((uint32_t)y * ch) / th);
        if (sy >= sh) sy = sh - 1;
        for (uint16_t x = 0; x < tw; ++x) {
            uint16_t sx = (uint16_t)(x0 + ((uint32_t)x * cw) / tw);
            if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * tw + x] = poster_index(src[(size_t)sy * sw + sx]);
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

esp_err_t prisma_process_run_poster(void)
{
    if (!begin_test("poster")) return ESP_ERR_INVALID_STATE;
    int64_t started = esp_timer_get_time();

    prisma_image_info_t info = {0};
    if (prisma_image_ab_get_info(&info) != ESP_OK || !info.valid || !info.size) {
        status_error("poster", "no active IMAGE in A/B storage");
        return ESP_ERR_NOT_FOUND;
    }

    prisma_poster_config_t pcfg;
    (void)prisma_poster_config_load(&pcfg);
    uint32_t cfg_hash = prisma_poster_config_hash(&pcfg, false);

    prisma_processed_info_t cached = {0}, preview = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &cached);
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_PREVIEW, &preview);
    if (cached.valid && preview.valid && cached.source_generation == info.generation &&
        preview.source_generation == info.generation && cached.config_hash == cfg_hash &&
        preview.config_hash == cfg_hash) {
        commit_flash_status("poster", 0, 0, cached.width, cached.height, 0,
                            info.generation, 1, cached.payload_size);
        return ESP_OK;
    }

    uint8_t *jpeg = psram_alloc(info.size);
    if (!jpeg) { status_error("poster", "not enough memory for JPEG source"); return ESP_ERR_NO_MEM; }
    esp_err_t err = prisma_image_ab_read(0, jpeg, info.size);
    if (err != ESP_OK) { free(jpeg); status_error("poster", "could not read active IMAGE"); return err; }

    uint16_t sw=0, sh=0;
    if (!jpeg_baseline_size(jpeg, info.size, &sw, &sh)) {
        free(jpeg); status_error("poster", "JPEG must be baseline (non-progressive)"); return ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t tw=0, th=0;
    if (!prisma_poster_resolve_output(pcfg.image_preset, pcfg.image_aspect, sw, sh, &tw, &th)) {
        free(jpeg); status_error("poster", "invalid IMAGE output configuration"); return ESP_ERR_INVALID_ARG;
    }

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t budget = free_psram > PROCESS_PSRAM_RESERVE ? free_psram - PROCESS_PSRAM_RESERVE : 0;
    uint8_t div = choose_decode_div(sw, sh, tw, th, budget);
    if (!div) { free(jpeg); status_error("poster", "IMAGE output exceeds safe PSRAM workspace"); return ESP_ERR_NO_MEM; }

    uint16_t dw = sw / div, dh = sh / div;
    size_t decode_bytes = (size_t)dw * dh * sizeof(uint16_t);
    uint16_t *decode = psram_alloc(decode_bytes);
    uint8_t *index = psram_alloc((size_t)tw * th);
    if (!decode || !index) {
        free(jpeg); free(decode); free(index);
        status_error("poster", "not enough PSRAM for temporary IMAGE buffers");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg, .indata_size = info.size,
        .outbuf = (uint8_t *)decode, .outbuf_size = decode_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = scale_enum(div),
        .flags = {.swap_color_bytes = 0},
    };
    esp_jpeg_image_output_t decoded = {0};
    err = esp_jpeg_decode(&cfg, &decoded);
    free(jpeg);
    if (err != ESP_OK) {
        free(decode); free(index); status_error("poster", "ESP JPEG decoder failed"); return err;
    }

    rgb565_to_index(decode, decoded.width, decoded.height, index, tw, th);
    free(decode);

    err = store_index(PRISMA_PROCESSED_IMAGE_OUT, index, tw, th, info.generation, cfg_hash);
    if (err != ESP_OK) {
        free(index); status_error("poster", "could not commit IMAGE OUT to flash"); return err;
    }

    uint16_t pw=0, ph=0;
    if (!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166, pcfg.image_aspect, sw, sh, &pw, &ph)) {
        free(index); status_error("poster", "could not resolve 166P preview"); return ESP_FAIL;
    }
    uint8_t *small = psram_alloc((size_t)pw * ph);
    if (!small) { free(index); status_error("poster", "not enough PSRAM for 166P preview"); return ESP_ERR_NO_MEM; }
    scale_index(index, tw, th, small, pw, ph);
    free(index);
    err = store_index(PRISMA_PROCESSED_IMAGE_PREVIEW, small, pw, ph, info.generation, cfg_hash);
    free(small);
    if (err != ESP_OK) { status_error("poster", "could not commit IMAGE PREVIEW 166 to flash"); return err; }

    prisma_processed_info_t out = {0};
    (void)prisma_processed_get_info(PRISMA_PROCESSED_IMAGE_OUT, &out);
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    commit_flash_status("poster", sw, sh, tw, th, elapsed, info.generation, div, out.payload_size);
    ESP_LOGI(TAG, "POSTER source=%ux%u output=%ux%u preview=%ux%u div=%u %lums",
             sw, sh, tw, th, pw, ph, div, (unsigned long)elapsed);
    return ESP_OK;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static uint16_t *new_canvas(const char *name)
{
    uint16_t *px = psram_alloc(TEST_W * TEST_H * sizeof(uint16_t));
    if (!px) { status_error(name, "not enough RAM/PSRAM for generator"); return NULL; }
    uint16_t bg = rgb565(4,7,12);
    for (size_t i=0;i<TEST_W*TEST_H;i++) px[i]=bg;
    return px;
}

static void draw_line(uint16_t *px,int w,int h,int x0,int y0,int x1,int y1,uint16_t c)
{
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
    for(;;){ if((unsigned)x0<(unsigned)w&&(unsigned)y0<(unsigned)h)px[y0*w+x0]=c;
        if(x0==x1&&y0==y1)break; int e2=2*e; if(e2>=dy){e+=dy;x0+=sx;} if(e2<=dx){e+=dx;y0+=sy;} }
}
static void draw_rect(uint16_t *p,int x,int y,int w,int h,uint16_t c){draw_line(p,TEST_W,TEST_H,x,y,x+w,y,c);draw_line(p,TEST_W,TEST_H,x+w,y,x+w,y+h,c);draw_line(p,TEST_W,TEST_H,x+w,y+h,x,y+h,c);draw_line(p,TEST_W,TEST_H,x,y+h,x,y,c);}
static void plot8(uint16_t *p,int cx,int cy,int x,int y,uint16_t c){int a[8][2]={{cx+x,cy+y},{cx-x,cy+y},{cx+x,cy-y},{cx-x,cy-y},{cx+y,cy+x},{cx-y,cy+x},{cx+y,cy-x},{cx-y,cy-x}};for(int i=0;i<8;i++)if((unsigned)a[i][0]<TEST_W&&(unsigned)a[i][1]<TEST_H)p[a[i][1]*TEST_W+a[i][0]]=c;}
static void draw_circle(uint16_t *p,int cx,int cy,int r,uint16_t c){int x=r,y=0,e=1-r;while(x>=y){plot8(p,cx,cy,x,y,c);y++;if(e<0)e+=2*y+1;else{x--;e+=2*(y-x)+1;}}}
static void finish_generator(const char *n,uint16_t *p,int64_t s){uint32_t ms=(uint32_t)((esp_timer_get_time()-s)/1000);commit_generator(n,p,ms);}

esp_err_t prisma_process_run_lines(void){if(!begin_test("lines"))return ESP_ERR_INVALID_STATE;int64_t s=esp_timer_get_time();uint16_t*p=new_canvas("lines");if(!p)return ESP_ERR_NO_MEM;uint16_t a=rgb565(40,215,215),b=rgb565(240,245,250);for(int i=0;i<18;i++){int y=8+i*13;draw_line(p,TEST_W,TEST_H,8,y,TEST_W-9,TEST_H-1-y,(i&1)?a:b);}for(int i=0;i<8;i++){int x=18+i*29;draw_line(p,TEST_W,TEST_H,x,8,TEST_W-1-x,TEST_H-9,a);}finish_generator("lines",p,s);return ESP_OK;}
esp_err_t prisma_process_run_rectangles(void){if(!begin_test("rectangles"))return ESP_ERR_INVALID_STATE;int64_t s=esp_timer_get_time();uint16_t*p=new_canvas("rectangles");if(!p)return ESP_ERR_NO_MEM;uint16_t a=rgb565(40,215,215),b=rgb565(240,245,250);for(int i=0;i<9;i++){int z=10+i*10;draw_rect(p,z,z,TEST_W-1-2*z,TEST_H-1-2*z,(i&1)?a:b);}finish_generator("rectangles",p,s);return ESP_OK;}
esp_err_t prisma_process_run_circles(void){if(!begin_test("circles"))return ESP_ERR_INVALID_STATE;int64_t s=esp_timer_get_time();uint16_t*p=new_canvas("circles");if(!p)return ESP_ERR_NO_MEM;uint16_t a=rgb565(40,215,215),b=rgb565(240,245,250);for(int r=14,i=0;r<=105;r+=13,i++)draw_circle(p,120,120,r,(i&1)?a:b);finish_generator("circles",p,s);return ESP_OK;}
esp_err_t prisma_process_run_triangles(void){if(!begin_test("triangles"))return ESP_ERR_INVALID_STATE;int64_t s=esp_timer_get_time();uint16_t*p=new_canvas("triangles");if(!p)return ESP_ERR_NO_MEM;uint16_t a=rgb565(40,215,215),b=rgb565(240,245,250);for(int i=0;i<9;i++){int z=10+i*10;uint16_t c=(i&1)?a:b;draw_line(p,TEST_W,TEST_H,120,z,TEST_W-1-z,TEST_H-1-z,c);draw_line(p,TEST_W,TEST_H,TEST_W-1-z,TEST_H-1-z,z,TEST_H-1-z,c);draw_line(p,TEST_W,TEST_H,z,TEST_H-1-z,120,z,c);}finish_generator("triangles",p,s);return ESP_OK;}

esp_err_t prisma_process_get_status(prisma_process_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!g_lock && prisma_process_init() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock, portMAX_DELAY); *out=g_status;
    out->free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM); xSemaphoreGive(g_lock); return ESP_OK;
}

static void le16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);} static void le32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

static esp_err_t send_index_bmp(httpd_req_t *req, prisma_processed_slot_t slot)
{
    prisma_processed_info_t in={0}; esp_err_t err=prisma_processed_get_info(slot,&in);
    if(err!=ESP_OK||!in.valid||in.format!=PRISMA_PROCESSED_FMT_INDEX8)return ESP_ERR_NOT_FOUND;
    uint16_t w=in.width,h=in.height; uint32_t po=54u+64u*4u,rowb=((uint32_t)w+3u)&~3u,img=rowb*h,file=po+img;
    uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';le32(&hdr[2],file);le32(&hdr[10],po);le32(&hdr[14],40);le32(&hdr[18],w);le32(&hdr[22],h);le16(&hdr[26],1);le16(&hdr[28],8);le32(&hdr[34],img);le32(&hdr[46],64);le32(&hdr[50],64);
    static const uint8_t rb[4]={0,82,173,255},gg[4]={0,85,170,255};uint8_t pal[256];for(int i=0;i<64;i++){uint8_t r=(i>>4)&3,g=(i>>2)&3,b=i&3;pal[i*4]=rb[b];pal[i*4+1]=gg[g];pal[i*4+2]=rb[r];pal[i*4+3]=0;}
    uint8_t*row=malloc(rowb);if(!row)return ESP_ERR_NO_MEM;httpd_resp_set_type(req,"image/bmp");httpd_resp_set_hdr(req,"Cache-Control","no-store");err=httpd_resp_send_chunk(req,(const char*)hdr,sizeof(hdr));if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)pal,sizeof(pal));
    for(int y=h-1;err==ESP_OK&&y>=0;y--){memset(row,0,rowb);err=prisma_processed_read(slot,(size_t)y*w,row,w);if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)row,rowb);}free(row);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);return err;
}

static esp_err_t send_generator_bmp(httpd_req_t *req)
{
    xSemaphoreTake(g_lock,portMAX_DELAY); if(!g_status.valid||!g_pixels){xSemaphoreGive(g_lock);return ESP_ERR_NOT_FOUND;}uint16_t w=g_status.width,h=g_status.height;uint32_t rowb=((uint32_t)w*3u+3u)&~3u,img=rowb*h,file=54u+img;uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';le32(&hdr[2],file);le32(&hdr[10],54);le32(&hdr[14],40);le32(&hdr[18],w);le32(&hdr[22],h);le16(&hdr[26],1);le16(&hdr[28],24);le32(&hdr[34],img);uint8_t*row=malloc(rowb);if(!row){xSemaphoreGive(g_lock);return ESP_ERR_NO_MEM;}httpd_resp_set_type(req,"image/bmp");httpd_resp_set_hdr(req,"Cache-Control","no-store");esp_err_t err=httpd_resp_send_chunk(req,(const char*)hdr,sizeof(hdr));for(int y=h-1;err==ESP_OK&&y>=0;y--){memset(row,0,rowb);for(uint16_t x=0;x<w;x++){uint16_t c=g_pixels[(size_t)y*w+x];row[x*3]=(uint8_t)((c&31)*255u/31u);row[x*3+1]=(uint8_t)(((c>>5)&63)*255u/63u);row[x*3+2]=(uint8_t)(((c>>11)&31)*255u/31u);}err=httpd_resp_send_chunk(req,(const char*)row,rowb);}free(row);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);xSemaphoreGive(g_lock);return err;
}

esp_err_t prisma_process_send_bmp(httpd_req_t *req)
{
    if(!req)return ESP_ERR_INVALID_ARG;if(!g_lock&&prisma_process_init()!=ESP_OK)return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_lock,portMAX_DELAY);bool flash=g_flash_result&&g_status.valid;xSemaphoreGive(g_lock);
    return flash?send_index_bmp(req,PRISMA_PROCESSED_IMAGE_OUT):send_generator_bmp(req);
}

esp_err_t prisma_process_send_preview_bmp(httpd_req_t *req)
{
    if(!req)return ESP_ERR_INVALID_ARG;return send_index_bmp(req,PRISMA_PROCESSED_IMAGE_PREVIEW);
}
