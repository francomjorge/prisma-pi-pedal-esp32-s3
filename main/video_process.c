#include "video_process.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

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

static const char *TAG = "PRISMA_VIDEO_PROCESS";

typedef struct { size_t offset; size_t length; } frame_ref_t;
typedef struct __attribute__((packed)) { uint32_t length; uint8_t encoding; uint8_t reserved[3]; } frame_record_t;

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

static esp_jpeg_image_scale_t scale_enum(uint8_t div)
{
    switch (div) {
        case 2: return JPEG_IMAGE_SCALE_1_2;
        case 4: return JPEG_IMAGE_SCALE_1_4;
        case 8: return JPEG_IMAGE_SCALE_1_8;
        default: return JPEG_IMAGE_SCALE_0;
    }
}

static inline uint8_t level5(uint16_t v){if(v<=5)return 0;if(v<=15)return 1;if(v<=25)return 2;return 3;}
static inline uint8_t level6(uint16_t v){if(v<=10)return 0;if(v<=31)return 1;if(v<=52)return 2;return 3;}
static inline uint8_t poster_index(uint16_t c){return (uint8_t)((level5((c>>11)&31)<<4)|(level6((c>>5)&63)<<2)|level5(c&31));}

static void crop_for_target(uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,uint16_t*x0,uint16_t*y0,uint16_t*cw,uint16_t*ch)
{
    *x0=0;*y0=0;*cw=sw;*ch=sh;uint32_t lhs=(uint32_t)sw*th,rhs=(uint32_t)sh*tw;
    if(lhs>rhs){uint16_t w=(uint16_t)(((uint32_t)sh*tw)/th);if(!w)w=1;*cw=w;*x0=(uint16_t)((sw-w)/2u);}
    else if(lhs<rhs){uint16_t h=(uint16_t)(((uint32_t)sw*th)/tw);if(!h)h=1;*ch=h;*y0=(uint16_t)((sh-h)/2u);}
}

static void to_index(const uint16_t*src,uint16_t sw,uint16_t sh,uint8_t*dst,uint16_t tw,uint16_t th)
{
    uint16_t x0,y0,cw,ch;crop_for_target(sw,sh,tw,th,&x0,&y0,&cw,&ch);
    for(uint16_t y=0;y<th;y++){uint16_t sy=(uint16_t)(y0+((uint32_t)y*ch)/th);if(sy>=sh)sy=sh-1;
        for(uint16_t x=0;x<tw;x++){uint16_t sx=(uint16_t)(x0+((uint32_t)x*cw)/tw);if(sx>=sw)sx=sw-1;dst[(size_t)y*tw+x]=poster_index(src[(size_t)sy*sw+sx]);}}
}

static uint8_t choose_div(uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,size_t budget)
{
    const uint8_t ds[]={8,4,2,1};size_t out=(size_t)tw*th;uint16_t nw=tw<sw?tw:sw,nh=th<sh?th:sh;
    for(size_t i=0;i<sizeof(ds);i++){uint8_t d=ds[i];uint16_t w=sw/d,h=sh/d;if(!w||!h||w<nw||h<nh)continue;if((size_t)w*h*2u+out<=budget)return d;}
    for(size_t i=0;i<sizeof(ds);i++){uint8_t d=ds[i];uint16_t w=sw/d,h=sh/d;if(w&&h&&(size_t)w*h*2u+out<=budget)return d;}return 0;
}

static esp_err_t scan_frames(size_t source_size, frame_ref_t refs[MAX_SOURCE_FRAMES], uint16_t *count)
{
    uint8_t buf[SCAN_CHUNK];size_t off=0,soi=SIZE_MAX;uint8_t prev=0;bool have=false;uint16_t n=0;
    while(off<source_size){size_t chunk=source_size-off;if(chunk>sizeof(buf))chunk=sizeof(buf);esp_err_t err=prisma_video_ab_read(off,buf,chunk);if(err!=ESP_OK)return err;
        for(size_t i=0;i<chunk;i++){uint8_t b=buf[i];size_t pos=off+i;if(have){if(soi==SIZE_MAX&&prev==0xFF&&b==0xD8)soi=pos-1;else if(soi!=SIZE_MAX&&prev==0xFF&&b==0xD9){if(n>=MAX_SOURCE_FRAMES){*count=n;return ESP_OK;}refs[n].offset=soi;refs[n].length=(pos+1)-soi;n++;soi=SIZE_MAX;}}prev=b;have=true;}off+=chunk;}
    *count=n;return ESP_OK;
}

static size_t rle_size(const uint8_t *p,size_t n)
{
    size_t out=0,i=0;while(i<n){uint8_t v=p[i];size_t run=1;while(i+run<n&&p[i+run]==v&&run<255)run++;out+=2;i+=run;}return out;
}

static esp_err_t write_rle(prisma_processed_slot_t slot,size_t *off,const uint8_t*p,size_t n)
{
    uint8_t buf[RLE_CHUNK];size_t used=0,i=0;while(i<n){uint8_t v=p[i];size_t run=1;while(i+run<n&&p[i+run]==v&&run<255)run++;if(used+2>sizeof(buf)){esp_err_t e=prisma_processed_write(slot,*off,buf,used);if(e!=ESP_OK)return e;*off+=used;used=0;}buf[used++]=(uint8_t)run;buf[used++]=v;i+=run;}
    if(used){esp_err_t e=prisma_processed_write(slot,*off,buf,used);if(e!=ESP_OK)return e;*off+=used;}return ESP_OK;
}

static esp_err_t build_slot(prisma_processed_slot_t slot,const frame_ref_t*refs,uint16_t count,
                            uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,
                            uint32_t generation,uint32_t cfg_hash,uint16_t *stored_out,uint32_t *interval_out)
{
    if(!count)return ESP_ERR_NOT_FOUND;size_t raw=(size_t)tw*th,cap=prisma_processed_capacity(slot);if(cap<=sizeof(frame_record_t)||raw==0)return ESP_ERR_INVALID_SIZE;
    size_t max_frames=cap/(raw+sizeof(frame_record_t));if(max_frames<1)max_frames=1;if(max_frames>count)max_frames=count;
    uint16_t stride=(uint16_t)((count+max_frames-1)/max_frames);uint16_t expected=(uint16_t)((count+stride-1)/stride);

    size_t max_jpeg=0;for(uint16_t i=0;i<count;i=(uint16_t)(i+stride))if(refs[i].length>max_jpeg)max_jpeg=refs[i].length;
    uint8_t*jpeg=psram_alloc(max_jpeg);if(!jpeg)return ESP_ERR_NO_MEM;
    size_t free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM),budget=free_psram>VIDEO_PSRAM_RESERVE?free_psram-VIDEO_PSRAM_RESERVE:0;
    uint8_t div=choose_div(sw,sh,tw,th,budget);if(!div){free(jpeg);return ESP_ERR_NO_MEM;}
    uint16_t dw=sw/div,dh=sh/div;uint16_t*decode=psram_alloc((size_t)dw*dh*2u);uint8_t*index=psram_alloc(raw);
    if(!decode||!index){free(jpeg);free(decode);free(index);return ESP_ERR_NO_MEM;}

    esp_err_t err=prisma_processed_begin(slot,PRISMA_PROCESSED_FMT_VIDEO_INDEX8,generation,cfg_hash,tw,th);size_t outoff=0;uint16_t stored=0;
    if(err!=ESP_OK){free(jpeg);free(decode);free(index);return err;}
    for(uint16_t fi=0;fi<count&&err==ESP_OK;fi=(uint16_t)(fi+stride)){
        frame_ref_t r=refs[fi];if(r.length>max_jpeg){err=ESP_ERR_INVALID_SIZE;break;}err=prisma_video_ab_read(r.offset,jpeg,r.length);if(err!=ESP_OK)break;
        uint16_t fw=0,fh=0;if(!baseline_size(jpeg,r.length,&fw,&fh)){err=ESP_ERR_NOT_SUPPORTED;break;}
        esp_jpeg_image_cfg_t cfg={.indata=jpeg,.indata_size=r.length,.outbuf=(uint8_t*)decode,.outbuf_size=(size_t)dw*dh*2u,.out_format=JPEG_IMAGE_FORMAT_RGB565,.out_scale=scale_enum(div),.flags={.swap_color_bytes=0}};esp_jpeg_image_output_t d={0};
        err=esp_jpeg_decode(&cfg,&d);if(err!=ESP_OK)break;to_index(decode,d.width,d.height,index,tw,th);
        size_t rs=rle_size(index,raw);frame_record_t rec={.length=(uint32_t)(rs<raw?rs:raw),.encoding=(uint8_t)(rs<raw?FRAME_ENC_RLE:FRAME_ENC_RAW),.reserved={0,0,0}};
        err=prisma_processed_write(slot,outoff,&rec,sizeof(rec));if(err!=ESP_OK)break;outoff+=sizeof(rec);
        if(rec.encoding==FRAME_ENC_RAW){err=prisma_processed_write(slot,outoff,index,raw);if(err==ESP_OK)outoff+=raw;}
        else err=write_rle(slot,&outoff,index,raw);
        if(err==ESP_OK)stored++;
    }
    free(jpeg);free(decode);free(index);
    if(err==ESP_OK){uint32_t interval=SOURCE_FRAME_MS*stride;err=prisma_processed_finalize(slot,stored,count,interval);if(err==ESP_OK){*stored_out=stored;*interval_out=interval;}}
    if(err!=ESP_OK)prisma_processed_abort(slot);return err;
}

static esp_err_t first_frame_size(const frame_ref_t *r,uint16_t *w,uint16_t *h)
{
    uint8_t*buf=psram_alloc(r->length);if(!buf)return ESP_ERR_NO_MEM;esp_err_t err=prisma_video_ab_read(r->offset,buf,r->length);if(err==ESP_OK&&!baseline_size(buf,r->length,w,h))err=ESP_ERR_NOT_SUPPORTED;free(buf);return err;
}

static void status_from_cache(const prisma_video_info_t*src,const prisma_processed_info_t*main,const prisma_processed_info_t*prev,uint16_t sw,uint16_t sh,uint32_t elapsed)
{
    xSemaphoreTake(g_lock,portMAX_DELAY);memset(&g_status,0,sizeof(g_status));g_status.ready=true;g_status.prepared=true;g_status.valid=true;g_status.source_generation=src->generation;g_status.source_bytes=src->size;g_status.source_frame_count=main->source_frame_count;g_status.frame_count=main->frame_count;g_status.preview_frame_count=prev->frame_count;g_status.source_width=sw;g_status.source_height=sh;g_status.width=main->width;g_status.height=main->height;g_status.preview_width=prev->width;g_status.preview_height=prev->height;g_status.frame_interval_ms=main->frame_interval_ms;g_status.preview_interval_ms=prev->frame_interval_ms;g_status.last_process_ms=elapsed;g_status.output_bytes=main->payload_size;g_status.preview_bytes=prev->payload_size;g_status.free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);
}

esp_err_t prisma_video_process_prepare(void)
{
    if(!g_lock&&prisma_video_process_init()!=ESP_OK)return ESP_ERR_NO_MEM;int64_t started=esp_timer_get_time();
    prisma_video_info_t src={0};if(prisma_video_ab_get_info(&src)!=ESP_OK||!src.valid||!src.size){set_error("no active VIDEO in A/B storage");return ESP_ERR_NOT_FOUND;}
    frame_ref_t refs[MAX_SOURCE_FRAMES];uint16_t count=0;esp_err_t err=scan_frames(src.size,refs,&count);if(err!=ESP_OK||!count){set_error("VIDEO payload has no complete JPEG frames");return err!=ESP_OK?err:ESP_ERR_NOT_SUPPORTED;}
    uint16_t sw=0,sh=0;err=first_frame_size(&refs[0],&sw,&sh);if(err!=ESP_OK){set_error("VIDEO frames must be baseline JPEG");return err;}
    prisma_poster_config_t cfg;(void)prisma_poster_config_load(&cfg);uint32_t hash=prisma_poster_config_hash(&cfg,true);
    prisma_processed_info_t main={0},prev={0};(void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT,&main);(void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW,&prev);
    if(main.valid&&prev.valid&&main.source_generation==src.generation&&prev.source_generation==src.generation&&main.config_hash==hash&&prev.config_hash==hash){status_from_cache(&src,&main,&prev,sw,sh,0);return ESP_OK;}

    uint16_t tw=0,th=0,pw=0,ph=0;if(!prisma_poster_resolve_output(cfg.video_preset,cfg.video_aspect,sw,sh,&tw,&th)||!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166,cfg.video_aspect,sw,sh,&pw,&ph)){set_error("invalid VIDEO output configuration");return ESP_ERR_INVALID_ARG;}
    uint16_t stored=0,pstored=0;uint32_t interval=0,pinterval=0;
    err=build_slot(PRISMA_PROCESSED_VIDEO_OUT,refs,count,sw,sh,tw,th,src.generation,hash,&stored,&interval);if(err!=ESP_OK){set_error(err==ESP_ERR_NO_MEM?"not enough PSRAM workspace for VIDEO":"could not build VIDEO OUT");return err;}
    err=build_slot(PRISMA_PROCESSED_VIDEO_PREVIEW,refs,count,sw,sh,pw,ph,src.generation,hash,&pstored,&pinterval);if(err!=ESP_OK){set_error("could not build VIDEO PREVIEW 166");return err;}
    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT,&main);(void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW,&prev);
    uint32_t elapsed=(uint32_t)((esp_timer_get_time()-started)/1000);status_from_cache(&src,&main,&prev,sw,sh,elapsed);
    ESP_LOGI(TAG,"VIDEO source=%u frames output=%ux%u/%u preview=%ux%u/%u %lums",count,tw,th,stored,pw,ph,pstored,(unsigned long)elapsed);return ESP_OK;
}

esp_err_t prisma_video_process_get_status(prisma_video_process_status_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;if(!g_lock&&prisma_video_process_init()!=ESP_OK)return ESP_ERR_NO_MEM;xSemaphoreTake(g_lock,portMAX_DELAY);*out=g_status;out->free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);return ESP_OK;
}

static esp_err_t load_frame(prisma_processed_slot_t slot,uint16_t index,uint8_t**pixels,uint16_t*w,uint16_t*h)
{
    prisma_processed_info_t in={0};esp_err_t err=prisma_processed_get_info(slot,&in);if(err!=ESP_OK||!in.valid||in.format!=PRISMA_PROCESSED_FMT_VIDEO_INDEX8)return ESP_ERR_NOT_FOUND;if(index>=in.frame_count)return ESP_ERR_INVALID_ARG;
    size_t off=0;frame_record_t rec={0};for(uint16_t i=0;i<=index;i++){err=prisma_processed_read(slot,off,&rec,sizeof(rec));if(err!=ESP_OK)return err;off+=sizeof(rec);if(rec.length>in.payload_size-off)return ESP_ERR_INVALID_SIZE;if(i<index)off+=rec.length;}
    size_t n=(size_t)in.width*in.height;uint8_t*out=psram_alloc(n);if(!out)return ESP_ERR_NO_MEM;
    if(rec.encoding==FRAME_ENC_RAW){if(rec.length!=n){free(out);return ESP_ERR_INVALID_SIZE;}err=prisma_processed_read(slot,off,out,n);}
    else if(rec.encoding==FRAME_ENC_RLE){uint8_t buf[RLE_CHUNK];size_t pos=0,written=0;while(pos<rec.length&&err==ESP_OK){size_t k=rec.length-pos;if(k>sizeof(buf))k=sizeof(buf);if(k&1u)k--;if(!k){err=ESP_ERR_INVALID_SIZE;break;}err=prisma_processed_read(slot,off+pos,buf,k);if(err!=ESP_OK)break;for(size_t j=0;j<k;j+=2){uint8_t run=buf[j],v=buf[j+1];if(!run||written+run>n){err=ESP_ERR_INVALID_SIZE;break;}memset(out+written,v,run);written+=run;}pos+=k;}if(err==ESP_OK&&written!=n)err=ESP_ERR_INVALID_SIZE;}
    else err=ESP_ERR_NOT_SUPPORTED;
    if(err!=ESP_OK){free(out);return err;}*pixels=out;*w=in.width;*h=in.height;
    xSemaphoreTake(g_lock,portMAX_DELAY);g_status.last_frame=index;g_status.free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);return ESP_OK;
}

static void le16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}static void le32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

esp_err_t prisma_video_process_send_indexed(httpd_req_t *req,uint16_t index,bool preview)
{
    if(!req)return ESP_ERR_INVALID_ARG;uint8_t*p=NULL;uint16_t w=0,h=0;esp_err_t err=load_frame(preview?PRISMA_PROCESSED_VIDEO_PREVIEW:PRISMA_PROCESSED_VIDEO_OUT,index,&p,&w,&h);if(err!=ESP_OK)return err;uint8_t head[4];le16(head,w);le16(head+2,h);httpd_resp_set_type(req,"application/octet-stream");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_hdr(req,"X-Prisma-Format","POSTER64-INDEX8");err=httpd_resp_send_chunk(req,(const char*)head,4);if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)p,(size_t)w*h);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);free(p);return err;
}

esp_err_t prisma_video_process_send_bmp(httpd_req_t *req,uint16_t index,bool preview)
{
    if(!req)return ESP_ERR_INVALID_ARG;uint8_t*p=NULL;uint16_t w=0,h=0;esp_err_t err=load_frame(preview?PRISMA_PROCESSED_VIDEO_PREVIEW:PRISMA_PROCESSED_VIDEO_OUT,index,&p,&w,&h);if(err!=ESP_OK)return err;
    uint32_t po=54u+256u,rowb=((uint32_t)w+3u)&~3u,img=rowb*h,file=po+img;uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';le32(&hdr[2],file);le32(&hdr[10],po);le32(&hdr[14],40);le32(&hdr[18],w);le32(&hdr[22],h);le16(&hdr[26],1);le16(&hdr[28],8);le32(&hdr[34],img);le32(&hdr[46],64);le32(&hdr[50],64);static const uint8_t rb[4]={0,82,173,255},gg[4]={0,85,170,255};uint8_t pal[256];for(int i=0;i<64;i++){uint8_t r=(i>>4)&3,g=(i>>2)&3,b=i&3;pal[i*4]=rb[b];pal[i*4+1]=gg[g];pal[i*4+2]=rb[r];pal[i*4+3]=0;}uint8_t*row=malloc(rowb);if(!row){free(p);return ESP_ERR_NO_MEM;}httpd_resp_set_type(req,"image/bmp");httpd_resp_set_hdr(req,"Cache-Control","no-store");err=httpd_resp_send_chunk(req,(const char*)hdr,sizeof(hdr));if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)pal,sizeof(pal));for(int y=h-1;err==ESP_OK&&y>=0;y--){memset(row,0,rowb);memcpy(row,p+(size_t)y*w,w);err=httpd_resp_send_chunk(req,(const char*)row,rowb);}free(row);free(p);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);return err;
}
