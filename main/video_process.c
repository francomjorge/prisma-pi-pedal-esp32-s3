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
#define SCAN_CHUNK_BYTES 4096u
#define RLE_CHUNK_BYTES 512u
#define FRAME_ENC_RAW 0u
#define FRAME_ENC_RLE 1u

typedef struct {
    size_t offset;
    size_t length;
} frame_ref_t;

typedef struct __attribute__((packed)) {
    uint32_t length;
    uint8_t encoding;
    uint8_t reserved[3];
} frame_record_t;

static const char *TAG = "PRISMA_VIDEO_CACHE";
static SemaphoreHandle_t g_lock;
static prisma_video_process_status_t g_status;

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return p;
}

static void *frame_alloc(size_t bytes, bool preview)
{
    void *p = NULL;
    if (preview) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

static bool baseline_size(const uint8_t *buf, size_t len,
                          uint16_t *width, uint16_t *height)
{
    if (!buf || !width || !height || len < 10 ||
        buf[0] != 0xFF || buf[1] != 0xD8) return false;
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
static inline uint8_t poster_index(uint16_t c){return (uint8_t)((level5((c>>11)&31u)<<4)|(level6((c>>5)&63u)<<2)|level5(c&31u));}

static void crop_for_target(uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,
                            uint16_t*x0,uint16_t*y0,uint16_t*cw,uint16_t*ch)
{
    *x0=0;*y0=0;*cw=sw;*ch=sh;
    uint32_t lhs=(uint32_t)sw*th,rhs=(uint32_t)sh*tw;
    if(lhs>rhs){uint16_t w=(uint16_t)(((uint32_t)sh*tw)/th);if(!w)w=1;*cw=w;*x0=(uint16_t)((sw-w)/2u);}
    else if(lhs<rhs){uint16_t h=(uint16_t)(((uint32_t)sw*th)/tw);if(!h)h=1;*ch=h;*y0=(uint16_t)((sh-h)/2u);}
}

static void rgb565_to_index(const uint16_t*src,uint16_t sw,uint16_t sh,
                            uint8_t*dst,uint16_t tw,uint16_t th)
{
    uint16_t x0,y0,cw,ch;crop_for_target(sw,sh,tw,th,&x0,&y0,&cw,&ch);
    for(uint16_t y=0;y<th;y++){
        uint16_t sy=(uint16_t)(y0+((uint32_t)y*ch)/th);if(sy>=sh)sy=sh-1;
        for(uint16_t x=0;x<tw;x++){
            uint16_t sx=(uint16_t)(x0+((uint32_t)x*cw)/tw);if(sx>=sw)sx=sw-1;
            dst[(size_t)y*tw+x]=poster_index(src[(size_t)sy*sw+sx]);
        }
    }
}

static uint8_t choose_div(uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,size_t budget)
{
    const uint8_t ds[]={8,4,2,1};uint16_t nw=tw<sw?tw:sw,nh=th<sh?th:sh;size_t indexed=(size_t)tw*th;
    for(size_t i=0;i<sizeof(ds);i++){uint8_t d=ds[i];uint16_t w=sw/d,h=sh/d;if(!w||!h||w<nw||h<nh)continue;if((size_t)w*h*2u+indexed<=budget)return d;}
    for(size_t i=0;i<sizeof(ds);i++){uint8_t d=ds[i];uint16_t w=sw/d,h=sh/d;if(w&&h&&(size_t)w*h*2u+indexed<=budget)return d;}
    return 0;
}

static esp_err_t index_video_from_flash(size_t total,frame_ref_t refs[MAX_SOURCE_FRAMES],uint16_t*out_count)
{
    uint8_t*chunk=malloc(SCAN_CHUNK_BYTES);if(!chunk)return ESP_ERR_NO_MEM;
    bool in_frame=false,have_prev=false;size_t soi=0;uint8_t prev=0;uint16_t count=0;
    for(size_t base=0;base<total&&count<MAX_SOURCE_FRAMES;base+=SCAN_CHUNK_BYTES){
        size_t len=total-base;if(len>SCAN_CHUNK_BYTES)len=SCAN_CHUNK_BYTES;esp_err_t err=prisma_video_ab_read(base,chunk,len);if(err!=ESP_OK){free(chunk);return err;}
        for(size_t i=0;i<len&&count<MAX_SOURCE_FRAMES;i++){
            uint8_t cur=chunk[i];size_t pos=base+i;
            if(have_prev){
                if(!in_frame&&prev==0xFF&&cur==0xD8){soi=pos-1;in_frame=true;}
                else if(in_frame&&prev==0xFF&&cur==0xD9){refs[count].offset=soi;refs[count].length=(pos+1)-soi;count++;in_frame=false;}
            }
            prev=cur;have_prev=true;
        }
    }
    free(chunk);*out_count=count;return ESP_OK;
}

static size_t rle_size(const uint8_t*p,size_t n)
{
    size_t out=0,i=0;while(i<n){uint8_t v=p[i];size_t run=1;while(i+run<n&&p[i+run]==v&&run<255)run++;out+=2;i+=run;}return out;
}

static esp_err_t write_rle(prisma_processed_slot_t slot,size_t*off,const uint8_t*p,size_t n)
{
    uint8_t buf[RLE_CHUNK_BYTES];size_t used=0,i=0;
    while(i<n){uint8_t v=p[i];size_t run=1;while(i+run<n&&p[i+run]==v&&run<255)run++;if(used+2>sizeof(buf)){esp_err_t e=prisma_processed_write(slot,*off,buf,used);if(e!=ESP_OK)return e;*off+=used;used=0;}buf[used++]=(uint8_t)run;buf[used++]=v;i+=run;}
    if(used){esp_err_t e=prisma_processed_write(slot,*off,buf,used);if(e!=ESP_OK)return e;*off+=used;}return ESP_OK;
}

static esp_err_t first_frame_size(const frame_ref_t*ref,uint16_t*w,uint16_t*h)
{
    uint8_t*jpeg=psram_alloc(ref->length);if(!jpeg)return ESP_ERR_NO_MEM;esp_err_t err=prisma_video_ab_read(ref->offset,jpeg,ref->length);if(err==ESP_OK&&!baseline_size(jpeg,ref->length,w,h))err=ESP_ERR_NOT_SUPPORTED;free(jpeg);return err;
}

static esp_err_t build_slot(prisma_processed_slot_t slot,const frame_ref_t*refs,uint16_t count,
                            uint16_t sw,uint16_t sh,uint16_t tw,uint16_t th,
                            uint32_t generation,uint32_t cfg_hash,uint16_t*stored_out,uint32_t*interval_out)
{
    if(!count||!tw||!th)return ESP_ERR_INVALID_ARG;size_t raw=(size_t)tw*th,cap=prisma_processed_capacity(slot);
    if(cap<=sizeof(frame_record_t)||raw+sizeof(frame_record_t)>cap)return ESP_ERR_INVALID_SIZE;
    size_t max_frames=cap/(raw+sizeof(frame_record_t));if(max_frames<1)return ESP_ERR_INVALID_SIZE;if(max_frames>count)max_frames=count;
    uint16_t stride=(uint16_t)((count+max_frames-1u)/max_frames);
    size_t max_jpeg=0;for(uint16_t i=0;i<count;i=(uint16_t)(i+stride))if(refs[i].length>max_jpeg)max_jpeg=refs[i].length;
    uint8_t*jpeg=psram_alloc(max_jpeg);if(!jpeg)return ESP_ERR_NO_MEM;
    size_t free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM),budget=free_psram>VIDEO_PSRAM_RESERVE?free_psram-VIDEO_PSRAM_RESERVE:0;
    uint8_t div=choose_div(sw,sh,tw,th,budget);if(!div){free(jpeg);return ESP_ERR_NO_MEM;}
    uint16_t dw=sw/div,dh=sh/div;size_t decode_bytes=(size_t)dw*dh*2u;uint16_t*decoded=psram_alloc(decode_bytes);uint8_t*indexed=psram_alloc(raw);
    if(!decoded||!indexed){free(jpeg);free(decoded);free(indexed);return ESP_ERR_NO_MEM;}
    esp_err_t err=prisma_processed_begin(slot,PRISMA_PROCESSED_FMT_VIDEO_INDEX8,generation,cfg_hash,tw,th);size_t outoff=0;uint16_t stored=0;
    if(err!=ESP_OK){free(jpeg);free(decoded);free(indexed);return err;}
    for(uint16_t fi=0;err==ESP_OK&&fi<count;fi=(uint16_t)(fi+stride)){
        frame_ref_t ref=refs[fi];if(ref.length>max_jpeg){err=ESP_ERR_INVALID_SIZE;break;}err=prisma_video_ab_read(ref.offset,jpeg,ref.length);if(err!=ESP_OK)break;
        uint16_t fw=0,fh=0;if(!baseline_size(jpeg,ref.length,&fw,&fh)){err=ESP_ERR_NOT_SUPPORTED;break;}
        esp_jpeg_image_cfg_t cfg={.indata=jpeg,.indata_size=ref.length,.outbuf=(uint8_t*)decoded,.outbuf_size=decode_bytes,.out_format=JPEG_IMAGE_FORMAT_RGB565,.out_scale=scale_enum(div),.flags={.swap_color_bytes=0}};esp_jpeg_image_output_t out={0};
        err=esp_jpeg_decode(&cfg,&out);if(err!=ESP_OK)break;rgb565_to_index(decoded,out.width,out.height,indexed,tw,th);
        size_t compressed=rle_size(indexed,raw);frame_record_t rec={.length=(uint32_t)(compressed<raw?compressed:raw),.encoding=(uint8_t)(compressed<raw?FRAME_ENC_RLE:FRAME_ENC_RAW),.reserved={0,0,0}};
        if(outoff+sizeof(rec)+rec.length>cap){err=ESP_ERR_INVALID_SIZE;break;}err=prisma_processed_write(slot,outoff,&rec,sizeof(rec));if(err!=ESP_OK)break;outoff+=sizeof(rec);
        if(rec.encoding==FRAME_ENC_RAW){err=prisma_processed_write(slot,outoff,indexed,raw);if(err==ESP_OK)outoff+=raw;}else err=write_rle(slot,&outoff,indexed,raw);
        if(err==ESP_OK)stored++;
    }
    free(jpeg);free(decoded);free(indexed);
    if(err==ESP_OK){uint32_t interval=SOURCE_FRAME_MS*stride;err=prisma_processed_finalize(slot,stored,count,interval);if(err==ESP_OK){if(stored_out)*stored_out=stored;if(interval_out)*interval_out=interval;}}
    if(err!=ESP_OK)prisma_processed_abort(slot);return err;
}

static void status_from_cache(const prisma_video_info_t*src,const prisma_processed_info_t*main,const prisma_processed_info_t*preview,uint16_t sw,uint16_t sh,uint32_t elapsed)
{
    xSemaphoreTake(g_lock,portMAX_DELAY);memset(&g_status,0,sizeof(g_status));g_status.ready=true;g_status.prepared=true;g_status.valid=true;
    g_status.source_generation=src?src->generation:main->source_generation;g_status.source_bytes=src?src->size:0;g_status.source_frame_count=main->source_frame_count;g_status.frame_count=main->frame_count;g_status.preview_frame_count=preview->frame_count;
    g_status.source_width=sw;g_status.source_height=sh;g_status.width=main->width;g_status.height=main->height;g_status.preview_width=preview->width;g_status.preview_height=preview->height;
    g_status.frame_interval_ms=main->frame_interval_ms;g_status.preview_interval_ms=preview->frame_interval_ms;g_status.last_process_ms=elapsed;g_status.output_bytes=main->payload_size;g_status.preview_bytes=preview->payload_size;g_status.free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);
}

static bool cached_pair_for_source(const prisma_video_info_t*src,prisma_processed_info_t*main,prisma_processed_info_t*preview)
{
    if(!src||!src->valid)return false;
    if(prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT,main)!=ESP_OK||prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW,preview)!=ESP_OK)return false;
    return main->valid&&preview->valid&&main->format==PRISMA_PROCESSED_FMT_VIDEO_INDEX8&&preview->format==PRISMA_PROCESSED_FMT_VIDEO_INDEX8&&main->source_generation==src->generation&&preview->source_generation==src->generation&&main->config_hash==preview->config_hash;
}

esp_err_t prisma_video_process_prepare(void)
{
    if(!g_lock&&prisma_video_process_init()!=ESP_OK)return ESP_ERR_NO_MEM;int64_t started=esp_timer_get_time();
    prisma_video_info_t src={0};if(prisma_video_ab_get_info(&src)!=ESP_OK||!src.valid||!src.size){set_error("no active VIDEO in A/B storage");return ESP_ERR_NOT_FOUND;}
    frame_ref_t refs[MAX_SOURCE_FRAMES];uint16_t count=0;esp_err_t err=index_video_from_flash(src.size,refs,&count);if(err!=ESP_OK||!count){set_error("VIDEO payload has no complete JPEG frames");return err!=ESP_OK?err:ESP_ERR_NOT_SUPPORTED;}
    uint16_t sw=0,sh=0;err=first_frame_size(&refs[0],&sw,&sh);if(err!=ESP_OK){set_error("VIDEO frames must be baseline JPEG");return err;}
    prisma_poster_config_t cfg;(void)prisma_poster_config_load(&cfg);uint32_t hash=prisma_poster_config_hash(&cfg,true);
    prisma_processed_info_t main={0},preview={0};if(cached_pair_for_source(&src,&main,&preview)&&main.config_hash==hash){status_from_cache(&src,&main,&preview,sw,sh,0);ESP_LOGI(TAG,"VIDEO cache already valid");return ESP_OK;}
    uint16_t tw=0,th=0,pw=0,ph=0;if(!prisma_poster_resolve_output(cfg.video_preset,cfg.video_aspect,sw,sh,&tw,&th)||!prisma_poster_resolve_output(PRISMA_POSTER_PRESET_166,cfg.video_aspect,sw,sh,&pw,&ph)){set_error("invalid VIDEO output configuration");return ESP_ERR_INVALID_ARG;}
    uint16_t stored=0,pstored=0;uint32_t interval=0,pinterval=0;
    err=build_slot(PRISMA_PROCESSED_VIDEO_OUT,refs,count,sw,sh,tw,th,src.generation,hash,&stored,&interval);if(err!=ESP_OK){set_error(err==ESP_ERR_NO_MEM?"not enough PSRAM for one-shot VIDEO preprocessing":"could not build VIDEO OUT cache");return err;}
    err=build_slot(PRISMA_PROCESSED_VIDEO_PREVIEW,refs,count,sw,sh,pw,ph,src.generation,hash,&pstored,&pinterval);if(err!=ESP_OK){(void)prisma_processed_invalidate(PRISMA_PROCESSED_VIDEO_OUT);set_error("could not build VIDEO PREVIEW cache");return err;}
    (void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT,&main);(void)prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW,&preview);uint32_t elapsed=(uint32_t)((esp_timer_get_time()-started)/1000);status_from_cache(&src,&main,&preview,sw,sh,elapsed);
    ESP_LOGI(TAG,"VIDEO preprocessed once source=%ux%u/%u output=%ux%u/%u preview=%ux%u/%u elapsed=%lums free_psram=%u",sw,sh,count,tw,th,stored,pw,ph,pstored,(unsigned long)elapsed,(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));return ESP_OK;
}

esp_err_t prisma_video_process_get_status(prisma_video_process_status_t*out)
{
    if(!out)return ESP_ERR_INVALID_ARG;if(!g_lock&&prisma_video_process_init()!=ESP_OK)return ESP_ERR_NO_MEM;
    prisma_video_info_t src={0};prisma_processed_info_t main={0},preview={0};
    if(prisma_video_ab_get_info(&src)==ESP_OK&&cached_pair_for_source(&src,&main,&preview)){
        xSemaphoreTake(g_lock,portMAX_DELAY);bool already=g_status.prepared&&g_status.source_generation==src.generation&&g_status.frame_count==main.frame_count&&g_status.preview_frame_count==preview.frame_count;xSemaphoreGive(g_lock);if(!already)status_from_cache(&src,&main,&preview,0,0,0);
    }
    xSemaphoreTake(g_lock,portMAX_DELAY);*out=g_status;out->free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);return ESP_OK;
}

static esp_err_t locate_frame(prisma_processed_slot_t slot,uint16_t index,prisma_processed_info_t*info,frame_record_t*rec,size_t*data_offset)
{
    esp_err_t err=prisma_processed_get_info(slot,info);if(err!=ESP_OK||!info->valid||info->format!=PRISMA_PROCESSED_FMT_VIDEO_INDEX8)return ESP_ERR_NOT_FOUND;if(index>=info->frame_count)return ESP_ERR_INVALID_ARG;
    size_t off=0;for(uint16_t i=0;i<=index;i++){err=prisma_processed_read(slot,off,rec,sizeof(*rec));if(err!=ESP_OK)return err;off+=sizeof(*rec);if(rec->length>info->payload_size||off>info->payload_size-rec->length)return ESP_ERR_INVALID_SIZE;if(i<index)off+=rec->length;}
    *data_offset=off;return ESP_OK;
}

static esp_err_t load_cached_frame(prisma_processed_slot_t slot,uint16_t index,bool preview,uint8_t**pixels,uint16_t*w,uint16_t*h)
{
    prisma_processed_info_t info={0};frame_record_t rec={0};size_t data_offset=0;esp_err_t err=locate_frame(slot,index,&info,&rec,&data_offset);if(err!=ESP_OK)return err;
    size_t n=(size_t)info.width*info.height;uint8_t*out=frame_alloc(n,preview);if(!out)return ESP_ERR_NO_MEM;
    if(rec.encoding==FRAME_ENC_RAW){if(rec.length!=n){free(out);return ESP_ERR_INVALID_SIZE;}err=prisma_processed_read(slot,data_offset,out,n);}
    else if(rec.encoding==FRAME_ENC_RLE){uint8_t buf[RLE_CHUNK_BYTES];size_t pos=0,written=0;while(pos<rec.length&&err==ESP_OK){size_t take=rec.length-pos;if(take>sizeof(buf))take=sizeof(buf);if(take&1u)take--;if(!take){err=ESP_ERR_INVALID_SIZE;break;}err=prisma_processed_read(slot,data_offset+pos,buf,take);if(err!=ESP_OK)break;for(size_t j=0;j<take;j+=2){uint8_t run=buf[j],v=buf[j+1];if(!run||written+run>n){err=ESP_ERR_INVALID_SIZE;break;}memset(out+written,v,run);written+=run;}pos+=take;}if(err==ESP_OK&&written!=n)err=ESP_ERR_INVALID_SIZE;}
    else err=ESP_ERR_NOT_SUPPORTED;
    if(err!=ESP_OK){free(out);return err;}*pixels=out;*w=info.width;*h=info.height;return ESP_OK;
}

static void frame_complete(uint16_t index,uint32_t elapsed)
{
    xSemaphoreTake(g_lock,portMAX_DELAY);g_status.last_frame=index;g_status.last_frame_ms=elapsed;g_status.free_psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);xSemaphoreGive(g_lock);
}

static void le16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}static void le32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

esp_err_t prisma_video_process_send_indexed(httpd_req_t*req,uint16_t index,bool preview)
{
    if(!req)return ESP_ERR_INVALID_ARG;int64_t started=esp_timer_get_time();uint8_t*p=NULL;uint16_t w=0,h=0;esp_err_t err=load_cached_frame(preview?PRISMA_PROCESSED_VIDEO_PREVIEW:PRISMA_PROCESSED_VIDEO_OUT,index,preview,&p,&w,&h);if(err!=ESP_OK)return err;
    uint8_t head[4];le16(head,w);le16(head+2,h);httpd_resp_set_type(req,"application/octet-stream");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_hdr(req,"X-Prisma-Format","POSTER64-INDEX8-CACHED");err=httpd_resp_send_chunk(req,(const char*)head,4);if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)p,(size_t)w*h);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);free(p);frame_complete(index,(uint32_t)((esp_timer_get_time()-started)/1000));return err;
}

esp_err_t prisma_video_process_send_bmp(httpd_req_t*req,uint16_t index,bool preview)
{
    if(!req)return ESP_ERR_INVALID_ARG;int64_t started=esp_timer_get_time();uint8_t*p=NULL;uint16_t w=0,h=0;esp_err_t err=load_cached_frame(preview?PRISMA_PROCESSED_VIDEO_PREVIEW:PRISMA_PROCESSED_VIDEO_OUT,index,preview,&p,&w,&h);if(err!=ESP_OK)return err;
    uint32_t po=54u+64u*4u,rowb=((uint32_t)w+3u)&~3u,img=rowb*h,file=po+img;uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';le32(&hdr[2],file);le32(&hdr[10],po);le32(&hdr[14],40);le32(&hdr[18],w);le32(&hdr[22],h);le16(&hdr[26],1);le16(&hdr[28],8);le32(&hdr[34],img);le32(&hdr[46],64);le32(&hdr[50],64);
    static const uint8_t rb[4]={0,82,173,255},gg[4]={0,85,170,255};uint8_t pal[256];for(int i=0;i<64;i++){uint8_t r=(i>>4)&3,g=(i>>2)&3,b=i&3;pal[i*4]=rb[b];pal[i*4+1]=gg[g];pal[i*4+2]=rb[r];pal[i*4+3]=0;}
    uint8_t*row=malloc(rowb);if(!row){free(p);return ESP_ERR_NO_MEM;}httpd_resp_set_type(req,"image/bmp");httpd_resp_set_hdr(req,"Cache-Control","no-store");err=httpd_resp_send_chunk(req,(const char*)hdr,sizeof(hdr));if(err==ESP_OK)err=httpd_resp_send_chunk(req,(const char*)pal,sizeof(pal));for(int y=h-1;err==ESP_OK&&y>=0;y--){memset(row,0,rowb);memcpy(row,p+(size_t)y*w,w);err=httpd_resp_send_chunk(req,(const char*)row,rowb);}free(row);free(p);if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);frame_complete(index,(uint32_t)((esp_timer_get_time()-started)/1000));return err;
}

static inline uint16_t palette_rgb565(uint8_t z)
{
    static const uint8_t rb[4]={0,82,173,255},gg[4]={0,85,170,255};uint8_t r=rb[(z>>4)&3u],g=gg[(z>>2)&3u],b=rb[z&3u];return (uint16_t)(((uint16_t)(r&0xF8)<<8)|((uint16_t)(g&0xFC)<<3)|(b>>3));
}

esp_err_t prisma_video_process_copy_cached_rgb565(uint16_t index,bool preview,uint16_t*dst,uint16_t dst_width,uint16_t dst_height,bool byte_swap,uint16_t*src_width,uint16_t*src_height)
{
    if(!dst||!dst_width||!dst_height)return ESP_ERR_INVALID_ARG;int64_t started=esp_timer_get_time();uint8_t*p=NULL;uint16_t sw=0,sh=0;esp_err_t err=load_cached_frame(preview?PRISMA_PROCESSED_VIDEO_PREVIEW:PRISMA_PROCESSED_VIDEO_OUT,index,preview,&p,&sw,&sh);if(err!=ESP_OK)return err;
    if(src_width)*src_width=sw;if(src_height)*src_height=sh;for(size_t i=0;i<(size_t)dst_width*dst_height;i++)dst[i]=0;
    uint32_t fw=dst_width,fh=((uint32_t)sh*dst_width)/sw;if(fh>dst_height){fh=dst_height;fw=((uint32_t)sw*dst_height)/sh;}if(!fw)fw=1;if(!fh)fh=1;uint16_t x0=(uint16_t)((dst_width-fw)/2u),y0=(uint16_t)((dst_height-fh)/2u);
    for(uint16_t y=0;y<fh;y++){uint16_t sy=(uint16_t)(((uint32_t)y*sh)/fh);if(sy>=sh)sy=sh-1;uint16_t*d=dst+(size_t)(y0+y)*dst_width+x0;for(uint16_t x=0;x<fw;x++){uint16_t sx=(uint16_t)(((uint32_t)x*sw)/fw);if(sx>=sw)sx=sw-1;uint16_t c=palette_rgb565(p[(size_t)sy*sw+sx]);if(byte_swap)c=(uint16_t)((c>>8)|(c<<8));d[x]=c;}}
    free(p);frame_complete(index,(uint32_t)((esp_timer_get_time()-started)/1000));return ESP_OK;
}
