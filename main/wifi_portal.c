#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include "wifi_portal.h"
#include "image_ab.h"
#include "video_ab.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "PRISMA_WIFI";
static volatile bool control_on = true;
static volatile int control_mode = 0;
static volatile int control_intensity = 61;
static volatile int control_generation = 1;
static char control_filename[32] = "F1_MOON.JPG";
static volatile bool control_effects[6] = {false,false,false,false,false,true};
static volatile int control_effect_values[6] = {50,50,50,50,50,50};

bool prisma_control_get_on(void){return control_on;}
int prisma_control_get_mode(void){return control_mode;}
int prisma_control_get_intensity(void){return control_intensity;}
int prisma_control_get_generation(void){return control_generation;}
void prisma_control_set_on(bool v){control_on=v;}
void prisma_control_set_mode(int v){control_mode=v?1:0;}
void prisma_control_set_intensity(int v){if(v<0)v=0;if(v>100)v=100;control_intensity=v;}
void prisma_control_set_generation(int v){control_generation=v;}
void prisma_control_set_filename(const char *v){if(v&&*v)strlcpy(control_filename,v,sizeof(control_filename));}
void prisma_control_set_effect(int effect,bool enabled){if(effect<0||effect>=6)return;if(effect==5)enabled=true;control_effects[effect]=enabled;}
bool prisma_control_get_effect(int effect){if(effect<0||effect>=6)return false;if(effect==5)return true;return control_effects[effect];}
void prisma_control_set_effect_value(int effect,int value){if(effect<0||effect>=6)return;if(value<0)value=0;if(value>100)value=100;control_effect_values[effect]=value;}
int prisma_control_get_effect_value(int effect){if(effect<0||effect>=6)return 0;return control_effect_values[effect];}

extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t web_style_css_start[] asm("_binary_style_css_start");
extern const uint8_t web_style_css_end[] asm("_binary_style_css_end");

static void no_cache(httpd_req_t *r){httpd_resp_set_hdr(r,"Cache-Control","no-store, no-cache, must-revalidate, max-age=0");httpd_resp_set_hdr(r,"Pragma","no-cache");httpd_resp_set_hdr(r,"Expires","0");}
static esp_err_t send_embedded(httpd_req_t*r,const char*type,const uint8_t*s,const uint8_t*e){httpd_resp_set_type(r,type);no_cache(r);return httpd_resp_send(r,(const char*)s,(ssize_t)(e-s));}
static esp_err_t page_get(httpd_req_t*r){return send_embedded(r,"text/html",web_index_html_start,web_index_html_end);}
static esp_err_t css_get(httpd_req_t*r){return send_embedded(r,"text/css",web_style_css_start,web_style_css_end);}

static esp_err_t state_get(httpd_req_t*r)
{
    static bool have_last = false;static bool last_on = false;static int last_mode = -1;static int last_intensity = -1;static char last_filename[32] = "";static bool last_effects[6] = {false,false,false,false,false,false};static int last_values[6] = {-1,-1,-1,-1,-1,-1};static int generation = 0;
    bool on = prisma_appliance_get_on();int mode = prisma_appliance_get_mode() ? 1 : 0;int intensity = prisma_appliance_get_intensity();const char *filename = prisma_appliance_get_filename();bool effects[6];int values[6];
    if (!filename || !*filename) filename = "F1_MOON.JPG";
    for (int i=0;i<6;i++) {effects[i] = prisma_appliance_get_effect(i);values[i] = prisma_appliance_get_effect_value(i);}effects[5] = true;
    bool changed = !have_last || last_on != on || last_mode != mode || last_intensity != intensity || strcmp(last_filename, filename) != 0;for (int i=0;i<6;i++) if (last_effects[i] != effects[i] || last_values[i] != values[i]) changed = true;
    if (changed) {generation++;if (generation <= 0) generation = 1;last_on = on;last_mode = mode;last_intensity = intensity;strlcpy(last_filename, filename, sizeof(last_filename));for (int i=0;i<6;i++) {last_effects[i] = effects[i];last_values[i] = values[i];}have_last = true;ESP_LOGI(TAG,"WEB STATE gen=%d on=%s source=%s intensity=%d mode=%s",generation,on ? "ON" : "OFF",filename,intensity,mode ? "BYPASS" : "RANDOM");}
    char b[640];int n = snprintf(b,sizeof(b),"{\"on\":%s,\"mode\":%d,\"intensity\":%d,\"generation\":%d,\"filename\":\"%s\",\"effects\":[%s,%s,%s,%s,%s,true],\"values\":[%d,%d,%d,%d,%d,%d]}",on ? "true" : "false", mode, intensity, generation, filename,effects[0]?"true":"false", effects[1]?"true":"false", effects[2]?"true":"false",effects[3]?"true":"false", effects[4]?"true":"false",values[0],values[1],values[2],values[3],values[4],values[5]);
    if (n < 0 || n >= (int)sizeof(b)) {httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"state overflow");return ESP_FAIL;}httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static bool source_name_ok(const char *name){if(!name||!*name||strlen(name)>63)return false;for(const unsigned char *p=(const unsigned char*)name;*p;p++)if(!(isalnum(*p)||*p=='_'||*p=='-'||*p=='.'))return false;return strstr(name,"..") == NULL;}
static esp_err_t source_get(httpd_req_t*r)
{
    const char *prefix="/source/";const char *name=r->uri+strlen(prefix);const char *q=strchr(name,'?');char clean[64];size_t n=q?(size_t)(q-name):strlen(name);
    if(n==0||n>=sizeof(clean)){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");return ESP_FAIL;}memcpy(clean,name,n);clean[n]='\0';if(!source_name_ok(clean)){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");return ESP_FAIL;}
    char path[80];int plen=snprintf(path,sizeof(path),"/storage/%s",clean);if(plen<0||plen>=(int)sizeof(path)){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");return ESP_FAIL;}FILE *f=fopen(path,"rb");if(!f){httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"Source not found");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");no_cache(r);char buf[1024];size_t got;esp_err_t err=ESP_OK;while((got=fread(buf,1,sizeof(buf),f))>0){err=httpd_resp_send_chunk(r,buf,got);if(err!=ESP_OK)break;}fclose(f);if(err==ESP_OK)err=httpd_resp_send_chunk(r,NULL,0);return err;
}

static esp_err_t image_info_get(httpd_req_t *r)
{
    prisma_image_info_t info;esp_err_t err=prisma_image_ab_get_info(&info);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE A/B unavailable");return err;}
    char b[192];int n=snprintf(b,sizeof(b),"{\"ready\":%s,\"valid\":%s,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.ready?"true":"false",info.valid?"true":"false",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    if(n<0||n>=(int)sizeof(b)){return ESP_FAIL;}
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t image_put(httpd_req_t *r)
{
    size_t total=(size_t)r->content_len;if(total<4||total>PRISMA_IMAGE_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_send(r,"IMAGE must be JPEG <= 1 MB",HTTPD_RESP_USE_STRLEN);}
    esp_err_t err=prisma_image_ab_begin(total);if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot start IMAGE upload");return err;}
    uint8_t buf[1024];size_t offset=0;uint8_t first0=0,first1=0,last0=0,last1=0;
    while(offset<total){size_t want=total-offset;if(want>sizeof(buf))want=sizeof(buf);int got=httpd_req_recv(r,(char*)buf,want);if(got==HTTPD_SOCK_ERR_TIMEOUT)continue;if(got<=0){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE upload interrupted");return ESP_FAIL;}for(int i=0;i<got;++i){size_t pos=offset+(size_t)i;if(pos==0)first0=buf[i];if(pos==1)first1=buf[i];last0=last1;last1=buf[i];}err=prisma_image_ab_write(offset,buf,(size_t)got);if(err!=ESP_OK){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE flash write failed");return err;}offset+=(size_t)got;}
    if(first0!=0xFF||first1!=0xD8||last0!=0xFF||last1!=0xD9){prisma_image_ab_abort();httpd_resp_set_status(r,"415 Unsupported Media Type");return httpd_resp_send(r,"Invalid JPEG markers; previous IMAGE remains active",HTTPD_RESP_USE_STRLEN);}
    err=prisma_image_ab_finalize();if(err!=ESP_OK){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE commit failed");return err;}
    prisma_image_info_t info;if(prisma_image_ab_get_info(&info)!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE committed but status unavailable");return ESP_FAIL;}char b[160];int n=snprintf(b,sizeof(b),"{\"ok\":true,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t image_get(httpd_req_t *r)
{
    prisma_image_info_t info;esp_err_t err=prisma_image_ab_get_info(&info);if(err!=ESP_OK||!info.valid){httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"No active IMAGE");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");no_cache(r);uint8_t buf[1024];size_t offset=0;while(offset<info.size){size_t take=info.size-offset;if(take>sizeof(buf))take=sizeof(buf);err=prisma_image_ab_read(offset,buf,take);if(err!=ESP_OK){httpd_resp_send_chunk(r,NULL,0);return err;}err=httpd_resp_send_chunk(r,(const char*)buf,take);if(err!=ESP_OK)return err;offset+=take;}return httpd_resp_send_chunk(r,NULL,0);
}

static esp_err_t image_delete(httpd_req_t *r)
{
    esp_err_t err=prisma_image_ab_delete();if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE delete failed");return err;}httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,"{\"ok\":true,\"valid\":false}",HTTPD_RESP_USE_STRLEN);
}

static esp_err_t video_info_get(httpd_req_t *r)
{
    prisma_video_info_t info;esp_err_t err=prisma_video_ab_get_info(&info);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO A/B unavailable");return err;}
    char b[192];int n=snprintf(b,sizeof(b),"{\"ready\":%s,\"valid\":%s,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.ready?"true":"false",info.valid?"true":"false",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    if(n<0||n>=(int)sizeof(b)){return ESP_FAIL;}
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t video_put(httpd_req_t *r)
{
    size_t total=(size_t)r->content_len;if(total==0||total>PRISMA_VIDEO_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_send(r,"VIDEO payload must be <= 3 MB",HTTPD_RESP_USE_STRLEN);}
    esp_err_t err=prisma_video_ab_begin(total);if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot start VIDEO upload");return err;}
    uint8_t buf[1024];size_t offset=0;
    while(offset<total){size_t want=total-offset;if(want>sizeof(buf))want=sizeof(buf);int got=httpd_req_recv(r,(char*)buf,want);if(got==HTTPD_SOCK_ERR_TIMEOUT)continue;if(got<=0){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO upload interrupted");return ESP_FAIL;}err=prisma_video_ab_write(offset,buf,(size_t)got);if(err!=ESP_OK){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO flash write failed");return err;}offset+=(size_t)got;}
    err=prisma_video_ab_finalize();if(err!=ESP_OK){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO commit failed");return err;}
    prisma_video_info_t info;if(prisma_video_ab_get_info(&info)!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO committed but status unavailable");return ESP_FAIL;}char b[160];int n=snprintf(b,sizeof(b),"{\"ok\":true,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t video_get(httpd_req_t *r)
{
    prisma_video_info_t info;esp_err_t err=prisma_video_ab_get_info(&info);if(err!=ESP_OK||!info.valid){httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"No active VIDEO");return ESP_FAIL;}
    httpd_resp_set_type(r,"application/octet-stream");no_cache(r);uint8_t buf[1024];size_t offset=0;while(offset<info.size){size_t take=info.size-offset;if(take>sizeof(buf))take=sizeof(buf);err=prisma_video_ab_read(offset,buf,take);if(err!=ESP_OK){httpd_resp_send_chunk(r,NULL,0);return err;}err=httpd_resp_send_chunk(r,(const char*)buf,take);if(err!=ESP_OK)return err;offset+=take;}return httpd_resp_send_chunk(r,NULL,0);
}

static esp_err_t video_delete(httpd_req_t *r)
{
    esp_err_t err=prisma_video_ab_delete();if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO delete failed");return err;}httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,"{\"ok\":true,\"valid\":false}",HTTPD_RESP_USE_STRLEN);
}

static void start_portal(void)
{
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);char ap[32];snprintf(ap,sizeof(ap),"PRISMA-PI-%02X%02X",mac[4],mac[5]);
    wifi_config_t cfg={0};strlcpy((char*)cfg.ap.ssid,ap,sizeof(cfg.ap.ssid));cfg.ap.ssid_len=strlen(ap);cfg.ap.max_connection=4;cfg.ap.authmode=WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&cfg));ESP_ERROR_CHECK(esp_wifi_start());
    httpd_config_t hc=HTTPD_DEFAULT_CONFIG();hc.uri_match_fn=httpd_uri_match_wildcard;hc.lru_purge_enable=true;hc.max_uri_handlers=12;httpd_handle_t s=NULL;
    if(httpd_start(&s,&hc)==ESP_OK){
        httpd_uri_t a={.uri="/",.method=HTTP_GET,.handler=page_get};
        httpd_uri_t b={.uri="/api/state",.method=HTTP_GET,.handler=state_get};
        httpd_uri_t c={.uri="/style.css",.method=HTTP_GET,.handler=css_get};
        httpd_uri_t d={.uri="/source/*",.method=HTTP_GET,.handler=source_get};
        httpd_uri_t e={.uri="/api/v1/image/info",.method=HTTP_GET,.handler=image_info_get};
        httpd_uri_t f={.uri="/api/v1/image",.method=HTTP_PUT,.handler=image_put};
        httpd_uri_t g={.uri="/api/v1/image",.method=HTTP_GET,.handler=image_get};
        httpd_uri_t h={.uri="/api/v1/image",.method=HTTP_DELETE,.handler=image_delete};
        httpd_uri_t i={.uri="/api/v1/video/info",.method=HTTP_GET,.handler=video_info_get};
        httpd_uri_t j={.uri="/api/v1/video",.method=HTTP_PUT,.handler=video_put};
        httpd_uri_t k={.uri="/api/v1/video",.method=HTTP_GET,.handler=video_get};
        httpd_uri_t l={.uri="/api/v1/video",.method=HTTP_DELETE,.handler=video_delete};
        httpd_register_uri_handler(s,&a);httpd_register_uri_handler(s,&b);httpd_register_uri_handler(s,&c);httpd_register_uri_handler(s,&d);
        httpd_register_uri_handler(s,&e);httpd_register_uri_handler(s,&f);httpd_register_uri_handler(s,&g);httpd_register_uri_handler(s,&h);
        httpd_register_uri_handler(s,&i);httpd_register_uri_handler(s,&j);httpd_register_uri_handler(s,&k);httpd_register_uri_handler(s,&l);
    }
    ESP_LOGI(TAG,"PRISMA PEDAL CORE-1D IMAGE+VIDEO API: %s - http://192.168.4.1",ap);
}

void prisma_wifi_start(void)
{
    esp_err_t r=nvs_flash_init();if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());ESP_ERROR_CHECK(nvs_flash_init());}
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());esp_netif_create_default_wifi_ap();wifi_init_config_t i=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&i));start_portal();
}
