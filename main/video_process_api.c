#include "video_process_api.h"

#include <stdlib.h>
#include <string.h>

#include "admin_auth.h"
#include "cJSON.h"
#include "stream_control.h"
#include "video_process.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "PRISMA_VIDEO_API";

static bool auth(httpd_req_t *r){ return prisma_admin_authorize(r); }

static esp_err_t send_status(httpd_req_t *r)
{
    prisma_video_process_status_t s = {0};
    esp_err_t err = prisma_video_process_get_status(&s);
    if (err != ESP_OK) return err;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"ready",s.ready);
    cJSON_AddBoolToObject(j,"prepared",s.prepared);
    cJSON_AddBoolToObject(j,"valid",s.valid);
    cJSON_AddStringToObject(j,"error",s.error);
    cJSON_AddNumberToObject(j,"source_generation",s.source_generation);
    cJSON_AddNumberToObject(j,"source_bytes",s.source_bytes);
    cJSON_AddNumberToObject(j,"frame_count",s.frame_count);
    cJSON_AddNumberToObject(j,"last_frame",s.last_frame);
    cJSON_AddNumberToObject(j,"width",s.width);
    cJSON_AddNumberToObject(j,"height",s.height);
    cJSON_AddNumberToObject(j,"last_frame_ms",s.last_frame_ms);
    cJSON_AddNumberToObject(j,"pixel_bytes",s.pixel_bytes);
    cJSON_AddNumberToObject(j,"free_psram",s.free_psram);
    cJSON_AddBoolToObject(j,"appliance_stream",prisma_appliance_stream_enabled());
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if(!txt) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    err=httpd_resp_sendstr(r,txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t appliance_stream_get(httpd_req_t *r)
{
    char query[64]={0}, val[16]={0};
    if(httpd_req_get_url_query_str(r,query,sizeof(query))==ESP_OK &&
       httpd_query_key_value(query,"enabled",val,sizeof(val))==ESP_OK){
        bool enabled = !strcmp(val,"1") || !strcmp(val,"true") || !strcmp(val,"on");
        prisma_appliance_stream_set(enabled);
        ESP_LOGI(TAG,"APPLIANCE STREAM -> %s",enabled?"ON":"OFF");
    }
    char body[64];
    snprintf(body,sizeof(body),"{\"ok\":true,\"enabled\":%s}",
             prisma_appliance_stream_enabled()?"true":"false");
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,body);
}

static esp_err_t prepare_get(httpd_req_t *r)
{
    esp_err_t err=prisma_video_process_prepare();
    if(err!=ESP_OK){
        prisma_video_process_status_t s={0}; prisma_video_process_get_status(&s);
        if(err==ESP_ERR_NOT_FOUND) httpd_resp_set_status(r,"404 Not Found");
        else if(err==ESP_ERR_NOT_SUPPORTED) httpd_resp_set_status(r,"415 Unsupported Media Type");
        else httpd_resp_set_status(r,"500 Internal Server Error");
        return httpd_resp_sendstr(r,s.error[0]?s.error:"video prepare failed");
    }
    return send_status(r);
}

static esp_err_t frame_get(httpd_req_t *r)
{
    char query[64]={0}, val[16]={0};
    if(httpd_req_get_url_query_str(r,query,sizeof(query))!=ESP_OK ||
       httpd_query_key_value(query,"i",val,sizeof(val))!=ESP_OK){
        httpd_resp_set_status(r,"400 Bad Request");
        return httpd_resp_sendstr(r,"missing frame index i");
    }
    long idx=strtol(val,NULL,10);
    if(idx<0 || idx>65535){
        httpd_resp_set_status(r,"400 Bad Request");
        return httpd_resp_sendstr(r,"invalid frame index");
    }
    esp_err_t err=prisma_video_process_render_frame((uint16_t)idx);
    if(err!=ESP_OK){
        httpd_resp_set_status(r,err==ESP_ERR_INVALID_ARG?"416 Range Not Satisfiable":"500 Internal Server Error");
        return httpd_resp_sendstr(r,"video frame processing failed");
    }
    return prisma_video_process_send_bmp(r);
}

static esp_err_t video_dispatch_get(httpd_req_t *r)
{
    if(!auth(r)) return ESP_OK;
    if(strstr(r->uri,"/appliance-stream")!=NULL) return appliance_stream_get(r);
    if(strncmp(r->uri, "/api/v1/process/test/video/prepare", 34)==0) return prepare_get(r);
    if(strncmp(r->uri, "/api/v1/process/test/video/status", 33)==0) return send_status(r);
    if(strncmp(r->uri, "/api/v1/process/test/video/frame.bmp", 36)==0) return frame_get(r);
    httpd_resp_set_status(r,"404 Not Found");
    return httpd_resp_sendstr(r,"unknown video test endpoint");
}

void prisma_video_process_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t video={
        .uri="/api/v1/process/test/video/*",
        .method=HTTP_GET,
        .handler=video_dispatch_get
    };
    esp_err_t e=httpd_register_uri_handler(server,&video);
    if(e!=ESP_OK) ESP_LOGE(TAG,"video wildcard route failed: %s",esp_err_to_name(e));
    else ESP_LOGI(TAG,"CORE-4D VIDEO wildcard route ready (flash indexed / lazy frames)");
}
