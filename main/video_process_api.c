#include "video_process_api.h"

#include <stdlib.h>
#include <string.h>

#include "admin_auth.h"
#include "cJSON.h"
#include "processed_store.h"
#include "video_ab.h"
#include "video_process.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "PRISMA_VIDEO_API";
static bool auth(httpd_req_t *r){ return prisma_admin_authorize(r); }

static void validate_active_cache(prisma_video_process_status_t *s)
{
    prisma_video_info_t src = {0};
    prisma_processed_info_t main = {0};
    prisma_processed_info_t preview = {0};
    bool source_ok = prisma_video_ab_get_info(&src) == ESP_OK && src.valid;
    bool cache_ok = source_ok &&
        prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_OUT,&main) == ESP_OK &&
        prisma_processed_get_info(PRISMA_PROCESSED_VIDEO_PREVIEW,&preview) == ESP_OK &&
        main.valid && preview.valid &&
        main.format == PRISMA_PROCESSED_FMT_VIDEO_INDEX8 &&
        preview.format == PRISMA_PROCESSED_FMT_VIDEO_INDEX8 &&
        main.source_generation == src.generation &&
        preview.source_generation == src.generation &&
        main.config_hash == preview.config_hash;

    if (source_ok) {
        s->source_generation = src.generation;
        s->source_bytes = src.size;
    }
    if (!cache_ok) {
        s->prepared = false;
        s->valid = false;
        s->source_frame_count = 0;
        s->frame_count = 0;
        s->preview_frame_count = 0;
        s->width = s->height = 0;
        s->preview_width = s->preview_height = 0;
        s->frame_interval_ms = 0;
        s->preview_interval_ms = 0;
        s->output_bytes = 0;
        s->preview_bytes = 0;
        s->last_frame = 0;
        s->last_frame_ms = 0;
    }
}

static esp_err_t send_status(httpd_req_t *r)
{
    prisma_video_process_status_t s = {0};
    esp_err_t err = prisma_video_process_get_status(&s);
    if (err != ESP_OK) return err;
    validate_active_cache(&s);

    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(j,"ready",s.ready);
    cJSON_AddBoolToObject(j,"prepared",s.prepared);
    cJSON_AddBoolToObject(j,"valid",s.valid);
    cJSON_AddStringToObject(j,"error",s.error);
    cJSON_AddNumberToObject(j,"source_generation",s.source_generation);
    cJSON_AddNumberToObject(j,"source_bytes",s.source_bytes);
    cJSON_AddNumberToObject(j,"source_frame_count",s.source_frame_count);
    cJSON_AddNumberToObject(j,"frame_count",s.frame_count);
    cJSON_AddNumberToObject(j,"preview_frame_count",s.preview_frame_count);
    cJSON_AddNumberToObject(j,"last_frame",s.last_frame);
    cJSON_AddNumberToObject(j,"source_width",s.source_width);
    cJSON_AddNumberToObject(j,"source_height",s.source_height);
    cJSON_AddNumberToObject(j,"width",s.width);
    cJSON_AddNumberToObject(j,"height",s.height);
    cJSON_AddNumberToObject(j,"preview_width",s.preview_width);
    cJSON_AddNumberToObject(j,"preview_height",s.preview_height);
    cJSON_AddNumberToObject(j,"frame_interval_ms",s.frame_interval_ms);
    cJSON_AddNumberToObject(j,"preview_interval_ms",s.preview_interval_ms);
    cJSON_AddNumberToObject(j,"last_process_ms",s.last_process_ms);
    cJSON_AddNumberToObject(j,"last_frame_ms",s.last_frame_ms);
    cJSON_AddNumberToObject(j,"output_bytes",s.output_bytes);
    cJSON_AddNumberToObject(j,"preview_bytes",s.preview_bytes);
    cJSON_AddNumberToObject(j,"free_psram",s.free_psram);

    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    err = httpd_resp_sendstr(r,txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t prepare_get(httpd_req_t *r)
{
    esp_err_t err = prisma_video_process_prepare();
    if (err != ESP_OK) {
        prisma_video_process_status_t s = {0};
        (void)prisma_video_process_get_status(&s);
        if (err == ESP_ERR_NOT_FOUND) httpd_resp_set_status(r,"404 Not Found");
        else if (err == ESP_ERR_NOT_SUPPORTED) httpd_resp_set_status(r,"415 Unsupported Media Type");
        else if (err == ESP_ERR_NO_MEM) httpd_resp_set_status(r,"507 Insufficient Storage");
        else httpd_resp_set_status(r,"500 Internal Server Error");
        return httpd_resp_sendstr(r,s.error[0]?s.error:"video preprocess failed");
    }
    return send_status(r);
}

static bool frame_index(httpd_req_t *r, uint16_t *index)
{
    char query[64] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(r,query,sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query,"i",value,sizeof(value)) != ESP_OK) return false;
    long idx = strtol(value,NULL,10);
    if (idx < 0 || idx > 65535) return false;
    *index = (uint16_t)idx;
    return true;
}

static esp_err_t cached_indexed_get(httpd_req_t *r, bool preview)
{
    uint16_t index = 0;
    if (!frame_index(r,&index)) {
        httpd_resp_set_status(r,"400 Bad Request");
        return httpd_resp_sendstr(r,"missing or invalid frame index i");
    }
    esp_err_t err = prisma_video_process_send_indexed(r,index,preview);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(r,"416 Range Not Satisfiable");
        return httpd_resp_sendstr(r,"frame index out of range");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(r,"404 Not Found");
        return httpd_resp_sendstr(r,"preprocessed video cache not found");
    }
    return err;
}

static esp_err_t cached_bmp_get(httpd_req_t *r, bool preview)
{
    uint16_t index = 0;
    if (!frame_index(r,&index)) {
        httpd_resp_set_status(r,"400 Bad Request");
        return httpd_resp_sendstr(r,"missing or invalid frame index i");
    }
    esp_err_t err = prisma_video_process_send_bmp(r,index,preview);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(r,"416 Range Not Satisfiable");
        return httpd_resp_sendstr(r,"frame index out of range");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(r,"404 Not Found");
        return httpd_resp_sendstr(r,"preprocessed video cache not found");
    }
    return err;
}

static esp_err_t video_dispatch_get(httpd_req_t *r)
{
    if (!auth(r)) return ESP_OK;
    if (strstr(r->uri,"/prepare") != NULL) return prepare_get(r);
    if (strstr(r->uri,"/status") != NULL) return send_status(r);
    if (strstr(r->uri,"/preview.idx") != NULL) return cached_indexed_get(r,true);
    if (strstr(r->uri,"/frame.idx") != NULL) return cached_indexed_get(r,false);
    if (strstr(r->uri,"/preview.bmp") != NULL) return cached_bmp_get(r,true);
    if (strstr(r->uri,"/frame.bmp") != NULL) return cached_bmp_get(r,false);
    httpd_resp_set_status(r,"404 Not Found");
    return httpd_resp_sendstr(r,"unknown video endpoint");
}

void prisma_video_process_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t video = {
        .uri = "/api/v1/process/test/video/*",
        .method = HTTP_GET,
        .handler = video_dispatch_get,
    };
    esp_err_t err = httpd_register_uri_handler(server,&video);
    if (err != ESP_OK) ESP_LOGE(TAG,"video wildcard route failed: %s",esp_err_to_name(err));
    else ESP_LOGI(TAG,"CORE-5H VIDEO cache routes ready: preprocess once / playback from flash");
}
