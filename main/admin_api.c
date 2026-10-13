#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "admin_api.h"
#include "admin_auth.h"
#include "image_ab.h"
#include "video_ab.h"
#include "processed_store.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#define WIFI_SCAN_LIMIT 20
static const char *TAG="PRISMA_ADMIN";
extern const uint8_t web_admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t web_admin_html_end[] asm("_binary_admin_html_end");

static void no_cache(httpd_req_t*r){httpd_resp_set_hdr(r,"Cache-Control","no-store, no-cache, must-revalidate, max-age=0");httpd_resp_set_hdr(r,"Pragma","no-cache");httpd_resp_set_hdr(r,"Expires","0");}
static esp_err_t send_json(httpd_req_t*r,cJSON*j){if(!j)return ESP_ERR_NO_MEM;char*t=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!t)return ESP_ERR_NO_MEM;httpd_resp_set_type(r,"application/json");no_cache(r);esp_err_t e=httpd_resp_sendstr(r,t);cJSON_free(t);return e;}
static bool require_admin(httpd_req_t*r){return prisma_admin_authorize(r);}
static esp_err_t page_get(httpd_req_t*r){if(!require_admin(r))return ESP_OK;httpd_resp_set_type(r,"text/html");no_cache(r);return httpd_resp_send(r,(const char*)web_admin_html_start,(ssize_t)(web_admin_html_end-web_admin_html_start));}

static void add_processed(cJSON*root,const char*name,prisma_processed_slot_t slot)
{
    prisma_processed_info_t i={0};bool ok=prisma_processed_get_info(slot,&i)==ESP_OK;cJSON*j=cJSON_AddObjectToObject(root,name);cJSON_AddBoolToObject(j,"ready",ok&&i.ready);cJSON_AddBoolToObject(j,"valid",ok&&i.valid);cJSON_AddNumberToObject(j,"size",ok?i.payload_size:0);cJSON_AddNumberToObject(j,"capacity",ok?i.payload_capacity:0);cJSON_AddNumberToObject(j,"source_generation",ok?i.source_generation:0);cJSON_AddNumberToObject(j,"width",ok?i.width:0);cJSON_AddNumberToObject(j,"height",ok?i.height:0);cJSON_AddNumberToObject(j,"frames",ok?i.frame_count:0);cJSON_AddNumberToObject(j,"source_frames",ok?i.source_frame_count:0);cJSON_AddNumberToObject(j,"frame_interval_ms",ok?i.frame_interval_ms:0);cJSON_AddNumberToObject(j,"format",ok?i.format:0);
}

static esp_err_t storage_get(httpd_req_t*r)
{
    if(!require_admin(r))return ESP_OK;cJSON*root=cJSON_CreateObject();cJSON_AddStringToObject(root,"scope","PRISMA-PI");
    prisma_image_info_t im={0};bool imok=prisma_image_ab_get_info(&im)==ESP_OK;cJSON*ij=cJSON_AddObjectToObject(root,"image");cJSON_AddBoolToObject(ij,"ready",imok&&im.ready);cJSON_AddBoolToObject(ij,"valid",imok&&im.valid);char ib[2]={imok?im.active_bank:'-',0};cJSON_AddStringToObject(ij,"active_bank",ib);cJSON_AddNumberToObject(ij,"generation",imok?im.generation:0);cJSON_AddNumberToObject(ij,"size",imok?im.size:0);cJSON_AddNumberToObject(ij,"capacity",PRISMA_IMAGE_MAX_BYTES);char ic[9];snprintf(ic,sizeof(ic),"%08lx",(unsigned long)(imok?im.crc32:0));cJSON_AddStringToObject(ij,"crc32",ic);
    prisma_video_info_t vi={0};bool viok=prisma_video_ab_get_info(&vi)==ESP_OK;cJSON*vj=cJSON_AddObjectToObject(root,"video");cJSON_AddBoolToObject(vj,"ready",viok&&vi.ready);cJSON_AddBoolToObject(vj,"valid",viok&&vi.valid);char vb[2]={viok?vi.active_bank:'-',0};cJSON_AddStringToObject(vj,"active_bank",vb);cJSON_AddNumberToObject(vj,"generation",viok?vi.generation:0);cJSON_AddNumberToObject(vj,"size",viok?vi.size:0);cJSON_AddNumberToObject(vj,"capacity",PRISMA_VIDEO_MAX_BYTES);char vc[9];snprintf(vc,sizeof(vc),"%08lx",(unsigned long)(viok?vi.crc32:0));cJSON_AddStringToObject(vj,"crc32",vc);
    add_processed(root,"image_out",PRISMA_PROCESSED_IMAGE_OUT);add_processed(root,"video_out",PRISMA_PROCESSED_VIDEO_OUT);add_processed(root,"image_preview",PRISMA_PROCESSED_IMAGE_PREVIEW);add_processed(root,"video_preview",PRISMA_PROCESSED_VIDEO_PREVIEW);return send_json(r,root);
}

static esp_err_t save_wifi(const char*ssid,const char*password){nvs_handle_t h;esp_err_t e=nvs_open("prisma_net",NVS_READWRITE,&h);if(e!=ESP_OK)return e;e=nvs_set_str(h,"ssid",ssid);if(e==ESP_OK)e=nvs_set_str(h,"password",password?password:"");if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;}
static esp_err_t clear_wifi(void){nvs_handle_t h;esp_err_t e=nvs_open("prisma_net",NVS_READWRITE,&h);if(e!=ESP_OK)return e;e=nvs_erase_all(h);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;}
static void restart_task(void*a){(void)a;vTaskDelay(pdMS_TO_TICKS(700));esp_restart();}

static esp_err_t wifi_scan_get(httpd_req_t*r){if(!require_admin(r))return ESP_OK;wifi_scan_config_t s={0};s.show_hidden=false;esp_err_t e=esp_wifi_scan_start(&s,true);if(e!=ESP_OK){httpd_resp_set_status(r,"503 Service Unavailable");return httpd_resp_sendstr(r,"Wi-Fi scan unavailable");}uint16_t n=0;if(esp_wifi_scan_get_ap_num(&n)!=ESP_OK)n=0;if(n>WIFI_SCAN_LIMIT)n=WIFI_SCAN_LIMIT;wifi_ap_record_t*rec=n?calloc(n,sizeof(*rec)):NULL;if(n&&!rec)return ESP_ERR_NO_MEM;uint16_t found=n;if(n&&esp_wifi_scan_get_ap_records(&found,rec)!=ESP_OK)found=0;cJSON*j=cJSON_CreateObject(),*a=cJSON_AddArrayToObject(j,"networks");for(uint16_t i=0;i<found;i++){cJSON*x=cJSON_CreateObject();cJSON_AddStringToObject(x,"ssid",(const char*)rec[i].ssid);cJSON_AddNumberToObject(x,"rssi",rec[i].rssi);cJSON_AddNumberToObject(x,"auth",rec[i].authmode);cJSON_AddItemToArray(a,x);}free(rec);return send_json(r,j);}

static esp_err_t recv_json(httpd_req_t*r,char*b,size_t cap,cJSON**out){if(r->content_len<=0||(size_t)r->content_len>=cap)return ESP_ERR_INVALID_SIZE;size_t o=0;while(o<(size_t)r->content_len){int n=httpd_req_recv(r,b+o,(size_t)r->content_len-o);if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;if(n<=0)return ESP_FAIL;o+=(size_t)n;}b[o]='\0';*out=cJSON_Parse(b);return *out?ESP_OK:ESP_ERR_INVALID_ARG;}

static esp_err_t wifi_connect_post(httpd_req_t*r){if(!require_admin(r))return ESP_OK;char b[256];cJSON*j=NULL;if(recv_json(r,b,sizeof(b),&j)!=ESP_OK){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid JSON");return ESP_FAIL;}cJSON*s=cJSON_GetObjectItemCaseSensitive(j,"ssid"),*p=cJSON_GetObjectItemCaseSensitive(j,"password");const char*ss=cJSON_IsString(s)?s->valuestring:NULL,*pw=cJSON_IsString(p)?p->valuestring:"";size_t sl=ss?strlen(ss):0,pl=strlen(pw);if(!sl||sl>32||pl>63){cJSON_Delete(j);httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"SSID/password length invalid");return ESP_FAIL;}esp_err_t e=save_wifi(ss,pw);cJSON_Delete(j);if(e!=ESP_OK)return e;httpd_resp_set_type(r,"application/json");no_cache(r);esp_err_t q=httpd_resp_sendstr(r,"{\"ok\":true,\"restarting\":true}");xTaskCreate(restart_task,"wifi_restart",2048,NULL,5,NULL);return q;}
static esp_err_t wifi_reset_post(httpd_req_t*r){if(!require_admin(r))return ESP_OK;esp_err_t e=clear_wifi();if(e!=ESP_OK&&e!=ESP_ERR_NVS_NOT_FOUND)return e;httpd_resp_set_type(r,"application/json");no_cache(r);esp_err_t q=httpd_resp_sendstr(r,"{\"ok\":true,\"restarting\":true}");xTaskCreate(restart_task,"wifi_reset",2048,NULL,5,NULL);return q;}

static esp_err_t password_post(httpd_req_t*r){if(!require_admin(r))return ESP_OK;char b[256];cJSON*j=NULL;if(recv_json(r,b,sizeof(b),&j)!=ESP_OK){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid JSON");return ESP_FAIL;}memset(b,0,sizeof(b));cJSON*n=cJSON_GetObjectItemCaseSensitive(j,"new_password"),*c=cJSON_GetObjectItemCaseSensitive(j,"confirm_password");const char*np=cJSON_IsString(n)?n->valuestring:NULL,*cp=cJSON_IsString(c)?c->valuestring:NULL;size_t l=np?strlen(np):0;if(!np||!cp||l<8||l>64||strcmp(np,cp)){cJSON_Delete(j);httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Password must match and be 8-64 characters");return ESP_FAIL;}esp_err_t e=prisma_admin_set_password(np);cJSON_Delete(j);if(e!=ESP_OK)return e;httpd_resp_set_type(r,"application/json");no_cache(r);return httpd_resp_sendstr(r,"{\"ok\":true,\"credentials_changed\":true}");}

static void invalidate_image_outputs(void){(void)prisma_processed_invalidate(PRISMA_PROCESSED_IMAGE_OUT);(void)prisma_processed_invalidate(PRISMA_PROCESSED_IMAGE_PREVIEW);}static void invalidate_video_outputs(void){(void)prisma_processed_invalidate(PRISMA_PROCESSED_VIDEO_OUT);(void)prisma_processed_invalidate(PRISMA_PROCESSED_VIDEO_PREVIEW);}

static esp_err_t image_put(httpd_req_t*r){if(!require_admin(r))return ESP_OK;size_t total=(size_t)r->content_len;if(total<4||total>PRISMA_IMAGE_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_sendstr(r,"IMAGE must be JPEG <= 1 MB");}esp_err_t e=prisma_image_ab_begin(total);if(e!=ESP_OK)return e;uint8_t buf[1024],f0=0,f1=0,l0=0,l1=0;size_t off=0;while(off<total){size_t k=total-off;if(k>sizeof(buf))k=sizeof(buf);int n=httpd_req_recv(r,(char*)buf,k);if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;if(n<=0){prisma_image_ab_abort();return ESP_FAIL;}for(int i=0;i<n;i++){size_t p=off+(size_t)i;if(p==0)f0=buf[i];if(p==1)f1=buf[i];l0=l1;l1=buf[i];}e=prisma_image_ab_write(off,buf,(size_t)n);if(e!=ESP_OK){prisma_image_ab_abort();return e;}off+=(size_t)n;}if(f0!=0xFF||f1!=0xD8||l0!=0xFF||l1!=0xD9){prisma_image_ab_abort();httpd_resp_set_status(r,"415 Unsupported Media Type");return httpd_resp_sendstr(r,"Invalid JPEG; previous IMAGE remains active");}e=prisma_image_ab_finalize();if(e!=ESP_OK){prisma_image_ab_abort();return e;}invalidate_image_outputs();prisma_image_info_t i={0};prisma_image_ab_get_info(&i);cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);char bank[2]={i.active_bank,0};cJSON_AddStringToObject(j,"active_bank",bank);cJSON_AddNumberToObject(j,"generation",i.generation);cJSON_AddNumberToObject(j,"size",i.size);return send_json(r,j);}
static esp_err_t image_delete(httpd_req_t*r){if(!require_admin(r))return ESP_OK;esp_err_t e=prisma_image_ab_delete();if(e!=ESP_OK)return e;invalidate_image_outputs();httpd_resp_set_type(r,"application/json");return httpd_resp_sendstr(r,"{\"ok\":true,\"valid\":false}");}

static esp_err_t video_put(httpd_req_t*r){if(!require_admin(r))return ESP_OK;size_t total=(size_t)r->content_len;if(!total||total>PRISMA_VIDEO_MAX_BYTES){httpd_resp_set_status(r,"413 Payload Too Large");return httpd_resp_sendstr(r,"VIDEO payload must be <= 3 MB");}esp_err_t e=prisma_video_ab_begin(total);if(e!=ESP_OK)return e;uint8_t buf[1024];size_t off=0;while(off<total){size_t k=total-off;if(k>sizeof(buf))k=sizeof(buf);int n=httpd_req_recv(r,(char*)buf,k);if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;if(n<=0){prisma_video_ab_abort();return ESP_FAIL;}e=prisma_video_ab_write(off,buf,(size_t)n);if(e!=ESP_OK){prisma_video_ab_abort();return e;}off+=(size_t)n;}e=prisma_video_ab_finalize();if(e!=ESP_OK){prisma_video_ab_abort();return e;}invalidate_video_outputs();prisma_video_info_t i={0};prisma_video_ab_get_info(&i);cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);char bank[2]={i.active_bank,0};cJSON_AddStringToObject(j,"active_bank",bank);cJSON_AddNumberToObject(j,"generation",i.generation);cJSON_AddNumberToObject(j,"size",i.size);return send_json(r,j);}
static esp_err_t video_delete(httpd_req_t*r){if(!require_admin(r))return ESP_OK;esp_err_t e=prisma_video_ab_delete();if(e!=ESP_OK)return e;invalidate_video_outputs();httpd_resp_set_type(r,"application/json");return httpd_resp_sendstr(r,"{\"ok\":true,\"valid\":false}");}

static void reg(httpd_handle_t s,const httpd_uri_t*u){esp_err_t e=httpd_register_uri_handler(s,u);if(e!=ESP_OK)ESP_LOGE(TAG,"route %s failed: %s",u->uri,esp_err_to_name(e));}
void prisma_admin_register_routes(httpd_handle_t s){static const httpd_uri_t page={.uri="/admin",.method=HTTP_GET,.handler=page_get},storage={.uri="/api/v1/admin/storage",.method=HTTP_GET,.handler=storage_get},password={.uri="/api/v1/admin/password",.method=HTTP_POST,.handler=password_post},scan={.uri="/api/v1/wifi/scan",.method=HTTP_GET,.handler=wifi_scan_get},connect={.uri="/api/v1/wifi/connect",.method=HTTP_POST,.handler=wifi_connect_post},reset={.uri="/api/v1/wifi/reset",.method=HTTP_POST,.handler=wifi_reset_post},iu={.uri="/api/v1/image",.method=HTTP_PUT,.handler=image_put},id={.uri="/api/v1/image",.method=HTTP_DELETE,.handler=image_delete},vu={.uri="/api/v1/video",.method=HTTP_PUT,.handler=video_put},vd={.uri="/api/v1/video",.method=HTTP_DELETE,.handler=video_delete};reg(s,&page);reg(s,&storage);reg(s,&password);reg(s,&scan);reg(s,&connect);reg(s,&reset);reg(s,&iu);reg(s,&id);reg(s,&vu);reg(s,&vd);ESP_LOGI(TAG,"CORE-5A PRISMA-PI admin ready");}
