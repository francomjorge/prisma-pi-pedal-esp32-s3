#include "video_process_api.h"

#include <stdlib.h>
#include <string.h>

#include "admin_auth.h"
#include "cJSON.h"
#include "video_process.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "PRISMA_VIDEO_API";
static bool auth(httpd_req_t *r){ return prisma_admin_authorize(r); }

static esp_err_t send_status(httpd_req_t *r)
{
    prisma_video_process_status_t s={0};esp_err_t err=prisma_video_process_get_status(&s);if(err!=ESP_OK)return err;
    cJSON*j=cJSON_CreateObject();if(!j)return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(j,"ready",s.ready);cJSON_AddBoolToObject(j,"prepared",s.prepared);cJSON_AddBoolToObject(j,"valid",s.valid);cJSON_AddStringToObject(j,"error",s.error);
    cJSON_AddNumberToObject(j,"source_generation",s.source_generation);cJSON_AddNumberToObject(j,"source_bytes",s.source_bytes);cJSON_AddNumberToObject(j,"source_frame_count",s.source_frame_count);cJSON_AddNumberToObject(j,"frame_count",s.frame_count);cJSON_AddNumberToObject(j,"preview_frame_count",s.preview_frame_count);cJSON_AddNumberToObject(j,"last_frame",s.last_frame);
    cJSON_AddNumberToObject(j,"source_width",s.source_width);cJSON_AddNumberToObject(j,"source_height",s.source_height);cJSON_AddNumberToObject(j,"width",s.width);cJSON_AddNumberToObject(j,"height",s.height);cJSON_AddNumberToObject(j,"preview_width",s.preview_width);cJSON_AddNumberToObject(j,"preview_height",s.preview_height);
    cJSON_AddNumberToObject(j,"frame_interval_ms",s.frame_interval_ms);cJSON_AddNumberToObject(j,"preview_interval_ms",s.preview_interval_ms);cJSON_AddNumberToObject(j,"last_process_ms",s.last_process_ms);cJSON_AddNumberToObject(j,"output_bytes",s.output_bytes);cJSON_AddNumberToObject(j,"preview_bytes",s.preview_bytes);cJSON_AddNumberToObject(j,"free_psram",s.free_psram);
    char*txt=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!txt)return ESP_ERR_NO_MEM;httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");err=httpd_resp_sendstr(r,txt);cJSON_free(txt);return err;
}

static esp_err_t prepare_get(httpd_req_t *r)
{
    esp_err_t err=prisma_video_process_prepare();if(err!=ESP_OK){prisma_video_process_status_t s={0};prisma_video_process_get_status(&s);if(err==ESP_ERR_NOT_FOUND)httpd_resp_set_status(r,"404 Not Found");else if(err==ESP_ERR_NOT_SUPPORTED)httpd_resp_set_status(r,"415 Unsupported Media Type");else if(err==ESP_ERR_NO_MEM)httpd_resp_set_status(r,"507 Insufficient Storage");else httpd_resp_set_status(r,"500 Internal Server Error");return httpd_resp_sendstr(r,s.error[0]?s.error:"video processing failed");}return send_status(r);
}

static esp_err_t frame_get(httpd_req_t *r,bool indexed,bool preview)
{
    char query[64]={0},val[16]={0};if(httpd_req_get_url_query_str(r,query,sizeof(query))!=ESP_OK||httpd_query_key_value(query,"i",val,sizeof(val))!=ESP_OK){httpd_resp_set_status(r,"400 Bad Request");return httpd_resp_sendstr(r,"missing frame index i");}
    long idx=strtol(val,NULL,10);if(idx<0||idx>65535){httpd_resp_set_status(r,"400 Bad Request");return httpd_resp_sendstr(r,"invalid frame index");}
    esp_err_t err=indexed?prisma_video_process_send_indexed(r,(uint16_t)idx,preview):prisma_video_process_send_bmp(r,(uint16_t)idx,preview);if(err==ESP_ERR_INVALID_ARG){httpd_resp_set_status(r,"416 Range Not Satisfiable");return httpd_resp_sendstr(r,"frame outside processed output");}if(err==ESP_ERR_NOT_FOUND){httpd_resp_set_status(r,"404 Not Found");return httpd_resp_sendstr(r,"processed video not ready");}return err;
}

static esp_err_t dispatch(httpd_req_t *r)
{
    if(!auth(r))return ESP_OK;
    if(strstr(r->uri,"/prepare"))return prepare_get(r);
    if(strstr(r->uri,"/status"))return send_status(r);
    bool preview=strstr(r->uri,"/preview.")!=NULL;
    if(strstr(r->uri,".idx"))return frame_get(r,true,preview);
    if(strstr(r->uri,".bmp"))return frame_get(r,false,preview);
    httpd_resp_set_status(r,"404 Not Found");return httpd_resp_sendstr(r,"unknown video endpoint");
}

void prisma_video_process_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t video={.uri="/api/v1/process/test/video/*",.method=HTTP_GET,.handler=dispatch};
    esp_err_t e=httpd_register_uri_handler(server,&video);if(e!=ESP_OK)ESP_LOGE(TAG,"video wildcard route failed: %s",esp_err_to_name(e));else ESP_LOGI(TAG,"CORE-5A flash-backed VIDEO route ready");
}
