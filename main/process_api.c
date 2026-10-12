#include "process_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "admin_auth.h"
#include "cJSON.h"
#include "local_process.h"
#include "poster_config.h"
#include "video_process_api.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "PRISMA_PROCESS_API";

extern const uint8_t web_process_html_start[] asm("_binary_process_html_start");
extern const uint8_t web_process_html_end[] asm("_binary_process_html_end");

static bool require_admin(httpd_req_t *r){ return prisma_admin_authorize(r); }

static esp_err_t process_page_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)web_process_html_start,
                           (ssize_t)(web_process_html_end - web_process_html_start));
}

static esp_err_t send_status_json(httpd_req_t *r)
{
    prisma_process_status_t s = {0};
    esp_err_t err = prisma_process_get_status(&s);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "ready", s.ready);
    cJSON_AddBoolToObject(root, "busy", s.busy);
    cJSON_AddBoolToObject(root, "valid", s.valid);
    cJSON_AddStringToObject(root, "last_test", s.last_test);
    cJSON_AddStringToObject(root, "error", s.error);
    cJSON_AddNumberToObject(root, "source_width", s.source_width);
    cJSON_AddNumberToObject(root, "source_height", s.source_height);
    cJSON_AddNumberToObject(root, "width", s.width);
    cJSON_AddNumberToObject(root, "height", s.height);
    cJSON_AddNumberToObject(root, "elapsed_ms", s.elapsed_ms);
    cJSON_AddNumberToObject(root, "source_generation", s.source_generation);
    cJSON_AddNumberToObject(root, "pixel_bytes", s.pixel_bytes);
    cJSON_AddNumberToObject(root, "free_psram", s.free_psram);
    cJSON_AddNumberToObject(root, "jpeg_scale_div", s.jpeg_scale_div);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    return err;
}

static esp_err_t run_and_reply(httpd_req_t *r, esp_err_t (*fn)(void), const char *fallback)
{
    esp_err_t err = fn();
    if (err == ESP_OK) return send_status_json(r);
    prisma_process_status_t s = {0};
    (void)prisma_process_get_status(&s);
    if (err == ESP_ERR_NOT_FOUND) httpd_resp_set_status(r, "404 Not Found");
    else if (err == ESP_ERR_INVALID_STATE) httpd_resp_set_status(r, "409 Conflict");
    else if (err == ESP_ERR_NOT_SUPPORTED) httpd_resp_set_status(r, "415 Unsupported Media Type");
    else if (err == ESP_ERR_NO_MEM) httpd_resp_set_status(r, "507 Insufficient Storage");
    else httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, s.error[0] ? s.error : fallback);
}

static esp_err_t process_dispatch_post(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    if (strcmp(r->uri, "/api/v1/process/test/poster") == 0)
        return run_and_reply(r, prisma_process_run_poster, "poster processing failed");
    if (strcmp(r->uri, "/api/v1/process/test/lines") == 0)
        return run_and_reply(r, prisma_process_run_lines, "line generation failed");
    if (strcmp(r->uri, "/api/v1/process/test/rectangles") == 0)
        return run_and_reply(r, prisma_process_run_rectangles, "rectangle generation failed");
    if (strcmp(r->uri, "/api/v1/process/test/circles") == 0)
        return run_and_reply(r, prisma_process_run_circles, "circle generation failed");
    if (strcmp(r->uri, "/api/v1/process/test/triangles") == 0)
        return run_and_reply(r, prisma_process_run_triangles, "triangle generation failed");
    httpd_resp_set_status(r, "404 Not Found");
    return httpd_resp_sendstr(r, "unknown local processing command");
}

static esp_err_t status_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    return send_status_json(r);
}

static esp_err_t result_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    esp_err_t err = prisma_process_send_bmp(r);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(r, "404 Not Found");
        return httpd_resp_sendstr(r, "No local processing result yet");
    }
    return err;
}

static cJSON *config_json(const prisma_poster_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *image = cJSON_AddObjectToObject(root, "image");
    cJSON_AddStringToObject(image, "mode",
                            cfg->image_mode == PRISMA_POSTER_IMAGE_SOURCE ? "source" : "manual");
    cJSON_AddNumberToObject(image, "width", cfg->image_width);
    cJSON_AddNumberToObject(image, "height", cfg->image_height);
    cJSON *video = cJSON_AddObjectToObject(root, "video");
    cJSON_AddStringToObject(video, "mode", "manual");
    cJSON_AddNumberToObject(video, "width", cfg->video_width);
    cJSON_AddNumberToObject(video, "height", cfg->video_height);
    return root;
}

static esp_err_t send_config(httpd_req_t *r, const prisma_poster_config_t *cfg)
{
    cJSON *root = config_json(cfg);
    if (!root) return ESP_ERR_NO_MEM;
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    return err;
}

static esp_err_t poster_config_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    prisma_poster_config_t cfg;
    esp_err_t err = prisma_poster_config_load(&cfg);
    if (err != ESP_OK) return err;
    return send_config(r, &cfg);
}

static esp_err_t poster_config_post(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    if (r->content_len <= 0 || r->content_len > 511) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "invalid poster config body");
    }

    char body[512];
    size_t off = 0;
    while (off < (size_t)r->content_len) {
        int got = httpd_req_recv(r, body + off, (size_t)r->content_len - off);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        off += (size_t)got;
    }
    body[off] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "invalid JSON");
    }

    prisma_poster_config_t cfg;
    (void)prisma_poster_config_load(&cfg);
    cJSON *image = cJSON_GetObjectItemCaseSensitive(root, "image");
    cJSON *video = cJSON_GetObjectItemCaseSensitive(root, "video");
    if (cJSON_IsObject(image)) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(image, "mode");
        cJSON *w = cJSON_GetObjectItemCaseSensitive(image, "width");
        cJSON *h = cJSON_GetObjectItemCaseSensitive(image, "height");
        if (cJSON_IsString(mode)) {
            if (strcmp(mode->valuestring, "source") == 0) cfg.image_mode = PRISMA_POSTER_IMAGE_SOURCE;
            else if (strcmp(mode->valuestring, "manual") == 0) cfg.image_mode = PRISMA_POSTER_IMAGE_MANUAL;
            else { cJSON_Delete(root); httpd_resp_set_status(r, "400 Bad Request"); return httpd_resp_sendstr(r, "invalid image mode"); }
        }
        if (cJSON_IsNumber(w)) cfg.image_width = (uint16_t)w->valueint;
        if (cJSON_IsNumber(h)) cfg.image_height = (uint16_t)h->valueint;
    }
    if (cJSON_IsObject(video)) {
        cJSON *w = cJSON_GetObjectItemCaseSensitive(video, "width");
        cJSON *h = cJSON_GetObjectItemCaseSensitive(video, "height");
        if (cJSON_IsNumber(w)) cfg.video_width = (uint16_t)w->valueint;
        if (cJSON_IsNumber(h)) cfg.video_height = (uint16_t)h->valueint;
    }
    cJSON_Delete(root);

    if (!prisma_poster_config_valid(&cfg)) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "poster output resolution out of range");
    }
    esp_err_t err = prisma_poster_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "could not save poster config");
    }
    return send_config(r, &cfg);
}

static void register_one(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "route register failed %s method=%d: %s", uri->uri, (int)uri->method, esp_err_to_name(err));
}

void prisma_process_register_routes(httpd_handle_t server)
{
    (void)prisma_process_init();
    static const httpd_uri_t page = {.uri="/admin/process", .method=HTTP_GET, .handler=process_page_get};
    static const httpd_uri_t command = {.uri="/api/v1/process/test/*", .method=HTTP_POST, .handler=process_dispatch_post};
    static const httpd_uri_t status = {.uri="/api/v1/process/test/status", .method=HTTP_GET, .handler=status_get};
    static const httpd_uri_t result = {.uri="/api/v1/process/test/result.bmp", .method=HTTP_GET, .handler=result_get};
    static const httpd_uri_t cfg_get = {.uri="/api/v1/poster/config", .method=HTTP_GET, .handler=poster_config_get};
    static const httpd_uri_t cfg_post = {.uri="/api/v1/poster/config", .method=HTTP_POST, .handler=poster_config_post};

    register_one(server, &page);
    register_one(server, &command);
    register_one(server, &status);
    register_one(server, &result);
    register_one(server, &cfg_get);
    register_one(server, &cfg_post);
    prisma_video_process_register_routes(server);
    ESP_LOGI(TAG, "CORE-4F generators + POSTER image/video configuration ready at /admin/process");
}
