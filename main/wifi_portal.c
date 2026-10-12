#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#include "wifi_portal.h"
#include "image_ab.h"
#include "video_ab.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define WIFI_MAX_RETRY     5
#define WIFI_SCAN_LIMIT    20

static const char *TAG = "PRISMA_WIFI";
static EventGroupHandle_t wifi_events;
static bool wifi_provisioning = false;
static bool wifi_connected = false;
static int wifi_retry = 0;
static char wifi_ssid[33] = "";
static char wifi_ip[16] = "";
static char device_id[20] = "";
static char provisioning_ssid[32] = "";

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

static void no_cache(httpd_req_t *r)
{
    httpd_resp_set_hdr(r,"Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(r,"Pragma","no-cache");
    httpd_resp_set_hdr(r,"Expires","0");
}

static esp_err_t send_embedded(httpd_req_t*r,const char*type,const uint8_t*s,const uint8_t*e)
{
    httpd_resp_set_type(r,type);
    no_cache(r);
    return httpd_resp_send(r,(const char*)s,(ssize_t)(e-s));
}

static esp_err_t send_json(httpd_req_t *r, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(r,"application/json");
    no_cache(r);
    esp_err_t err = httpd_resp_sendstr(r,text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t page_get(httpd_req_t*r){return send_embedded(r,"text/html",web_index_html_start,web_index_html_end);}
static esp_err_t css_get(httpd_req_t*r){return send_embedded(r,"text/css",web_style_css_start,web_style_css_end);}

static esp_err_t state_get(httpd_req_t*r)
{
    static bool have_last = false;
    static bool last_on = false;
    static int last_mode = -1;
    static int last_intensity = -1;
    static char last_filename[32] = "";
    static bool last_effects[6] = {false,false,false,false,false,false};
    static int last_values[6] = {-1,-1,-1,-1,-1,-1};
    static int generation = 0;

    bool on = prisma_appliance_get_on();
    int mode = prisma_appliance_get_mode() ? 1 : 0;
    int intensity = prisma_appliance_get_intensity();
    const char *filename = prisma_appliance_get_filename();
    bool effects[6];
    int values[6];
    if (!filename || !*filename) filename = "F1_MOON.JPG";
    for (int i=0;i<6;i++) {
        effects[i] = prisma_appliance_get_effect(i);
        values[i] = prisma_appliance_get_effect_value(i);
    }
    effects[5] = true;

    bool changed = !have_last || last_on != on || last_mode != mode ||
                   last_intensity != intensity || strcmp(last_filename, filename) != 0;
    for (int i=0;i<6;i++) {
        if (last_effects[i] != effects[i] || last_values[i] != values[i]) changed = true;
    }
    if (changed) {
        generation++;
        if (generation <= 0) generation = 1;
        last_on = on;
        last_mode = mode;
        last_intensity = intensity;
        strlcpy(last_filename, filename, sizeof(last_filename));
        for (int i=0;i<6;i++) {
            last_effects[i] = effects[i];
            last_values[i] = values[i];
        }
        have_last = true;
    }

    char b[640];
    int n = snprintf(b,sizeof(b),
        "{\"on\":%s,\"mode\":%d,\"intensity\":%d,\"generation\":%d,\"filename\":\"%s\",\"effects\":[%s,%s,%s,%s,%s,true],\"values\":[%d,%d,%d,%d,%d,%d]}",
        on ? "true" : "false", mode, intensity, generation, filename,
        effects[0]?"true":"false", effects[1]?"true":"false", effects[2]?"true":"false",
        effects[3]?"true":"false", effects[4]?"true":"false",
        values[0],values[1],values[2],values[3],values[4],values[5]);
    if (n < 0 || n >= (int)sizeof(b)) {
        httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"state overflow");
        return ESP_FAIL;
    }
    httpd_resp_set_type(r,"application/json");
    no_cache(r);
    return httpd_resp_send(r,b,n);
}

static bool source_name_ok(const char *name)
{
    if(!name||!*name||strlen(name)>63)return false;
    for(const unsigned char *p=(const unsigned char*)name;*p;p++) {
        if(!(isalnum(*p)||*p=='_'||*p=='-'||*p=='.'))return false;
    }
    return strstr(name,"..") == NULL;
}

static esp_err_t source_get(httpd_req_t*r)
{
    const char *prefix="/source/";
    const char *name=r->uri+strlen(prefix);
    const char *q=strchr(name,'?');
    char clean[64];
    size_t n=q?(size_t)(q-name):strlen(name);
    if(n==0||n>=sizeof(clean)){
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");
        return ESP_FAIL;
    }
    memcpy(clean,name,n);
    clean[n]='\0';
    if(!source_name_ok(clean)){
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");
        return ESP_FAIL;
    }
    char path[80];
    int plen=snprintf(path,sizeof(path),"/storage/%s",clean);
    if(plen<0||plen>=(int)sizeof(path)){
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid source");
        return ESP_FAIL;
    }
    FILE *f=fopen(path,"rb");
    if(!f){
        httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"Source not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(r,"image/jpeg");
    no_cache(r);
    char buf[1024];
    size_t got;
    esp_err_t err=ESP_OK;
    while((got=fread(buf,1,sizeof(buf),f))>0){
        err=httpd_resp_send_chunk(r,buf,got);
        if(err!=ESP_OK)break;
    }
    fclose(f);
    if(err==ESP_OK)err=httpd_resp_send_chunk(r,NULL,0);
    return err;
}

static esp_err_t image_info_get(httpd_req_t *r)
{
    prisma_image_info_t info;
    esp_err_t err=prisma_image_ab_get_info(&info);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE A/B unavailable");return err;}
    char b[192];
    int n=snprintf(b,sizeof(b),"{\"ready\":%s,\"valid\":%s,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.ready?"true":"false",info.valid?"true":"false",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    if(n<0||n>=(int)sizeof(b))return ESP_FAIL;
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t image_put(httpd_req_t *r)
{
    size_t total=(size_t)r->content_len;
    if(total<4||total>PRISMA_IMAGE_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_send(r,"IMAGE must be JPEG <= 1 MB",HTTPD_RESP_USE_STRLEN);}
    esp_err_t err=prisma_image_ab_begin(total);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot start IMAGE upload");return err;}
    uint8_t buf[1024];
    size_t offset=0;
    uint8_t first0=0,first1=0,last0=0,last1=0;
    while(offset<total){
        size_t want=total-offset;
        if(want>sizeof(buf))want=sizeof(buf);
        int got=httpd_req_recv(r,(char*)buf,want);
        if(got==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(got<=0){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE upload interrupted");return ESP_FAIL;}
        for(int i=0;i<got;++i){
            size_t pos=offset+(size_t)i;
            if(pos==0)first0=buf[i];
            if(pos==1)first1=buf[i];
            last0=last1;
            last1=buf[i];
        }
        err=prisma_image_ab_write(offset,buf,(size_t)got);
        if(err!=ESP_OK){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE flash write failed");return err;}
        offset+=(size_t)got;
    }
    if(first0!=0xFF||first1!=0xD8||last0!=0xFF||last1!=0xD9){
        prisma_image_ab_abort();
        httpd_resp_set_status(r,"415 Unsupported Media Type");
        return httpd_resp_send(r,"Invalid JPEG markers; previous IMAGE remains active",HTTPD_RESP_USE_STRLEN);
    }
    err=prisma_image_ab_finalize();
    if(err!=ESP_OK){prisma_image_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE commit failed");return err;}
    prisma_image_info_t info;
    if(prisma_image_ab_get_info(&info)!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE committed but status unavailable");return ESP_FAIL;}
    char b[160];
    int n=snprintf(b,sizeof(b),"{\"ok\":true,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t image_get(httpd_req_t *r)
{
    prisma_image_info_t info;
    esp_err_t err=prisma_image_ab_get_info(&info);
    if(err!=ESP_OK||!info.valid){httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"No active IMAGE");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");
    no_cache(r);
    uint8_t buf[1024];
    size_t offset=0;
    while(offset<info.size){
        size_t take=info.size-offset;
        if(take>sizeof(buf))take=sizeof(buf);
        err=prisma_image_ab_read(offset,buf,take);
        if(err!=ESP_OK){httpd_resp_send_chunk(r,NULL,0);return err;}
        err=httpd_resp_send_chunk(r,(const char*)buf,take);
        if(err!=ESP_OK)return err;
        offset+=take;
    }
    return httpd_resp_send_chunk(r,NULL,0);
}

static esp_err_t image_delete(httpd_req_t *r)
{
    esp_err_t err=prisma_image_ab_delete();
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"IMAGE delete failed");return err;}
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,"{\"ok\":true,\"valid\":false}",HTTPD_RESP_USE_STRLEN);
}

static esp_err_t video_info_get(httpd_req_t *r)
{
    prisma_video_info_t info;
    esp_err_t err=prisma_video_ab_get_info(&info);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO A/B unavailable");return err;}
    char b[192];
    int n=snprintf(b,sizeof(b),"{\"ready\":%s,\"valid\":%s,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.ready?"true":"false",info.valid?"true":"false",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    if(n<0||n>=(int)sizeof(b))return ESP_FAIL;
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t video_put(httpd_req_t *r)
{
    size_t total=(size_t)r->content_len;
    if(total==0||total>PRISMA_VIDEO_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_send(r,"VIDEO payload must be <= 3 MB",HTTPD_RESP_USE_STRLEN);}
    esp_err_t err=prisma_video_ab_begin(total);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot start VIDEO upload");return err;}
    uint8_t buf[1024];
    size_t offset=0;
    while(offset<total){
        size_t want=total-offset;
        if(want>sizeof(buf))want=sizeof(buf);
        int got=httpd_req_recv(r,(char*)buf,want);
        if(got==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(got<=0){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO upload interrupted");return ESP_FAIL;}
        err=prisma_video_ab_write(offset,buf,(size_t)got);
        if(err!=ESP_OK){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO flash write failed");return err;}
        offset+=(size_t)got;
    }
    err=prisma_video_ab_finalize();
    if(err!=ESP_OK){prisma_video_ab_abort();httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO commit failed");return err;}
    prisma_video_info_t info;
    if(prisma_video_ab_get_info(&info)!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO committed but status unavailable");return ESP_FAIL;}
    char b[160];
    int n=snprintf(b,sizeof(b),"{\"ok\":true,\"active_bank\":\"%c\",\"generation\":%lu,\"size\":%u,\"crc32\":\"%08lx\"}",info.active_bank,(unsigned long)info.generation,(unsigned)info.size,(unsigned long)info.crc32);
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,b,n);
}

static esp_err_t video_get(httpd_req_t *r)
{
    prisma_video_info_t info;
    esp_err_t err=prisma_video_ab_get_info(&info);
    if(err!=ESP_OK||!info.valid){httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"No active VIDEO");return ESP_FAIL;}
    httpd_resp_set_type(r,"application/octet-stream");
    no_cache(r);
    uint8_t buf[1024];
    size_t offset=0;
    while(offset<info.size){
        size_t take=info.size-offset;
        if(take>sizeof(buf))take=sizeof(buf);
        err=prisma_video_ab_read(offset,buf,take);
        if(err!=ESP_OK){httpd_resp_send_chunk(r,NULL,0);return err;}
        err=httpd_resp_send_chunk(r,(const char*)buf,take);
        if(err!=ESP_OK)return err;
        offset+=take;
    }
    return httpd_resp_send_chunk(r,NULL,0);
}

static esp_err_t video_delete(httpd_req_t *r)
{
    esp_err_t err=prisma_video_ab_delete();
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO delete failed");return err;}
    httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_send(r,"{\"ok\":true,\"valid\":false}",HTTPD_RESP_USE_STRLEN);
}

static bool load_wifi_credentials(char *ssid,size_t ssid_size,char *password,size_t password_size)
{
    nvs_handle_t h;
    if(nvs_open("prisma_net",NVS_READONLY,&h)!=ESP_OK)return false;
    size_t ssid_len=ssid_size;
    esp_err_t err=nvs_get_str(h,"ssid",ssid,&ssid_len);
    if(err!=ESP_OK||ssid[0]=='\0'){
        nvs_close(h);
        return false;
    }
    size_t pass_len=password_size;
    err=nvs_get_str(h,"password",password,&pass_len);
    if(err==ESP_ERR_NVS_NOT_FOUND)password[0]='\0';
    else if(err!=ESP_OK){nvs_close(h);return false;}
    nvs_close(h);
    return true;
}

static esp_err_t save_wifi_credentials(const char *ssid,const char *password)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("prisma_net",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_set_str(h,"ssid",ssid);
    if(err==ESP_OK)err=nvs_set_str(h,"password",password?password:"");
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t clear_wifi_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("prisma_net",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_erase_all(h);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    return err;
}

static void wifi_event_handler(void *arg,esp_event_base_t event_base,int32_t event_id,void *event_data)
{
    (void)arg;
    if(event_base==WIFI_EVENT&&event_id==WIFI_EVENT_STA_START){
        if(!wifi_provisioning)esp_wifi_connect();
    }else if(event_base==WIFI_EVENT&&event_id==WIFI_EVENT_STA_DISCONNECTED){
        wifi_connected=false;
        wifi_ip[0]='\0';
        if(!wifi_provisioning&&wifi_retry<WIFI_MAX_RETRY){
            wifi_retry++;
            esp_wifi_connect();
        }else if(!wifi_provisioning){
            xEventGroupSetBits(wifi_events,WIFI_FAILED_BIT);
        }
    }else if(event_base==IP_EVENT&&event_id==IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *event=(ip_event_got_ip_t*)event_data;
        snprintf(wifi_ip,sizeof(wifi_ip),IPSTR,IP2STR(&event->ip_info.ip));
        wifi_connected=true;
        wifi_retry=0;
        xEventGroupSetBits(wifi_events,WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG,"STA connected ssid=%s ip=%s",wifi_ssid,wifi_ip);
    }
}

static esp_err_t configure_provisioning_ap(bool wifi_already_started)
{
    wifi_provisioning=true;
    wifi_connected=false;
    wifi_ip[0]='\0';
    if(wifi_already_started){
        esp_wifi_disconnect();
        esp_wifi_stop();
    }

    wifi_config_t ap={0};
    strlcpy((char*)ap.ap.ssid,provisioning_ssid,sizeof(ap.ap.ssid));
    ap.ap.ssid_len=strlen(provisioning_ssid);
    ap.ap.max_connection=4;
    ap.ap.authmode=WIFI_AUTH_OPEN;

    esp_err_t err=esp_wifi_set_mode(WIFI_MODE_APSTA);
    if(err!=ESP_OK)return err;
    err=esp_wifi_set_config(WIFI_IF_AP,&ap);
    if(err!=ESP_OK)return err;
    err=esp_wifi_start();
    if(err==ESP_OK)ESP_LOGW(TAG,"Provisioning AP active: %s http://192.168.4.1",provisioning_ssid);
    return err;
}

static bool start_saved_sta(const char *ssid,const char *password)
{
    wifi_provisioning=false;
    wifi_connected=false;
    wifi_retry=0;
    wifi_ip[0]='\0';
    xEventGroupClearBits(wifi_events,WIFI_CONNECTED_BIT|WIFI_FAILED_BIT);

    wifi_config_t sta={0};
    strlcpy((char*)sta.sta.ssid,ssid,sizeof(sta.sta.ssid));
    strlcpy((char*)sta.sta.password,password,sizeof(sta.sta.password));
    sta.sta.threshold.authmode=WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable=true;
    sta.sta.pmf_cfg.required=false;
    strlcpy(wifi_ssid,ssid,sizeof(wifi_ssid));

    if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK)return false;
    if(esp_wifi_set_config(WIFI_IF_STA,&sta)!=ESP_OK)return false;
    if(esp_wifi_start()!=ESP_OK)return false;

    EventBits_t bits=xEventGroupWaitBits(wifi_events,WIFI_CONNECTED_BIT|WIFI_FAILED_BIT,pdFALSE,pdFALSE,pdMS_TO_TICKS(12000));
    return (bits&WIFI_CONNECTED_BIT)!=0;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static esp_err_t info_get(httpd_req_t *r)
{
    cJSON *root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"product","PRISMA-PI PEDAL CORE");
    cJSON_AddStringToObject(root,"firmware","0.5.0-core2a");
    cJSON_AddStringToObject(root,"device_id",device_id);
    return send_json(r,root);
}

static esp_err_t wifi_status_get(httpd_req_t *r)
{
    cJSON *root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"device_id",device_id);
    cJSON_AddStringToObject(root,"mode",wifi_provisioning?"provisioning":"sta");
    cJSON_AddBoolToObject(root,"connected",wifi_connected);
    cJSON_AddStringToObject(root,"ssid",wifi_connected?wifi_ssid:"");
    cJSON_AddStringToObject(root,"ip",wifi_connected?wifi_ip:"");
    cJSON_AddStringToObject(root,"ap_ssid",wifi_provisioning?provisioning_ssid:"");
    return send_json(r,root);
}

static esp_err_t wifi_scan_get(httpd_req_t *r)
{
    wifi_scan_config_t scan={0};
    scan.show_hidden=false;
    esp_err_t err=esp_wifi_scan_start(&scan,true);
    if(err!=ESP_OK){
        httpd_resp_set_status(r,"503 Service Unavailable");
        return httpd_resp_send(r,"Wi-Fi scan unavailable",HTTPD_RESP_USE_STRLEN);
    }

    uint16_t count=0;
    if(esp_wifi_scan_get_ap_num(&count)!=ESP_OK)count=0;
    if(count>WIFI_SCAN_LIMIT)count=WIFI_SCAN_LIMIT;
    wifi_ap_record_t *records=count?calloc(count,sizeof(*records)):NULL;
    if(count&&records==NULL)return ESP_ERR_NO_MEM;
    uint16_t found=count;
    if(count&&esp_wifi_scan_get_ap_records(&found,records)!=ESP_OK)found=0;

    cJSON *root=cJSON_CreateObject();
    cJSON *items=cJSON_AddArrayToObject(root,"networks");
    for(uint16_t i=0;i<found;i++){
        cJSON *item=cJSON_CreateObject();
        cJSON_AddStringToObject(item,"ssid",(const char*)records[i].ssid);
        cJSON_AddNumberToObject(item,"rssi",records[i].rssi);
        cJSON_AddNumberToObject(item,"auth",records[i].authmode);
        cJSON_AddItemToArray(items,item);
    }
    free(records);
    return send_json(r,root);
}

static esp_err_t wifi_connect_post(httpd_req_t *r)
{
    if(r->content_len<=0||r->content_len>255){
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid JSON body");
        return ESP_FAIL;
    }
    char body[256];
    size_t offset=0;
    while(offset<(size_t)r->content_len){
        int got=httpd_req_recv(r,body+offset,(size_t)r->content_len-offset);
        if(got==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(got<=0)return ESP_FAIL;
        offset+=(size_t)got;
    }
    body[offset]='\0';

    cJSON *root=cJSON_Parse(body);
    if(!root){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid JSON");return ESP_FAIL;}
    cJSON *ssid=cJSON_GetObjectItemCaseSensitive(root,"ssid");
    cJSON *password=cJSON_GetObjectItemCaseSensitive(root,"password");
    const char *ssid_text=cJSON_IsString(ssid)?ssid->valuestring:NULL;
    const char *password_text=cJSON_IsString(password)?password->valuestring:"";
    size_t ssid_len=ssid_text?strlen(ssid_text):0;
    size_t pass_len=strlen(password_text);
    if(ssid_len==0||ssid_len>32||pass_len>63){
        cJSON_Delete(root);
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"SSID/password length invalid");
        return ESP_FAIL;
    }
    esp_err_t err=save_wifi_credentials(ssid_text,password_text);
    cJSON_Delete(root);
    if(err!=ESP_OK){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Could not save Wi-Fi");return err;}

    httpd_resp_set_type(r,"application/json");
    no_cache(r);
    esp_err_t response=httpd_resp_sendstr(r,"{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task,"wifi_restart",2048,NULL,5,NULL);
    return response;
}

static esp_err_t wifi_reset_post(httpd_req_t *r)
{
    esp_err_t err=clear_wifi_credentials();
    if(err!=ESP_OK&&err!=ESP_ERR_NVS_NOT_FOUND){
        httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Could not reset Wi-Fi");
        return err;
    }
    httpd_resp_set_type(r,"application/json");
    no_cache(r);
    esp_err_t response=httpd_resp_sendstr(r,"{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task,"wifi_reset",2048,NULL,5,NULL);
    return response;
}

static void start_http_server(void)
{
    httpd_config_t hc=HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn=httpd_uri_match_wildcard;
    hc.lru_purge_enable=true;
    hc.max_uri_handlers=20;
    httpd_handle_t s=NULL;
    if(httpd_start(&s,&hc)!=ESP_OK){
        ESP_LOGE(TAG,"HTTP server failed");
        return;
    }

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
    httpd_uri_t m={.uri="/api/v1/info",.method=HTTP_GET,.handler=info_get};
    httpd_uri_t n={.uri="/api/v1/wifi/status",.method=HTTP_GET,.handler=wifi_status_get};
    httpd_uri_t o={.uri="/api/v1/wifi/scan",.method=HTTP_GET,.handler=wifi_scan_get};
    httpd_uri_t p={.uri="/api/v1/wifi/connect",.method=HTTP_POST,.handler=wifi_connect_post};
    httpd_uri_t q={.uri="/api/v1/wifi/reset",.method=HTTP_POST,.handler=wifi_reset_post};

    httpd_register_uri_handler(s,&a);httpd_register_uri_handler(s,&b);httpd_register_uri_handler(s,&c);httpd_register_uri_handler(s,&d);
    httpd_register_uri_handler(s,&e);httpd_register_uri_handler(s,&f);httpd_register_uri_handler(s,&g);httpd_register_uri_handler(s,&h);
    httpd_register_uri_handler(s,&i);httpd_register_uri_handler(s,&j);httpd_register_uri_handler(s,&k);httpd_register_uri_handler(s,&l);
    httpd_register_uri_handler(s,&m);httpd_register_uri_handler(s,&n);httpd_register_uri_handler(s,&o);httpd_register_uri_handler(s,&p);httpd_register_uri_handler(s,&q);
}

void prisma_wifi_start(void)
{
    esp_err_t r=nvs_flash_init();
    if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }else{
        ESP_ERROR_CHECK(r);
    }

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    snprintf(device_id,sizeof(device_id),"PRP-%02X%02X%02X",mac[3],mac[4],mac[5]);
    snprintf(provisioning_ssid,sizeof(provisioning_ssid),"PRISMA-PI-%02X%02X",mac[4],mac[5]);

    wifi_events=xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_event_handler,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_event_handler,NULL));

    char saved_ssid[33]={0};
    char saved_password[64]={0};
    bool have_credentials=load_wifi_credentials(saved_ssid,sizeof(saved_ssid),saved_password,sizeof(saved_password));
    bool sta_ok=false;
    if(have_credentials){
        ESP_LOGI(TAG,"Trying saved Wi-Fi: %s",saved_ssid);
        sta_ok=start_saved_sta(saved_ssid,saved_password);
    }

    if(!sta_ok){
        if(configure_provisioning_ap(have_credentials)!=ESP_OK){
            ESP_LOGE(TAG,"Could not start provisioning AP");
            return;
        }
    }

    start_http_server();
    ESP_LOGI(TAG,"CORE-2A ready device=%s mode=%s",device_id,wifi_provisioning?"PROVISIONING":"STA");
}
