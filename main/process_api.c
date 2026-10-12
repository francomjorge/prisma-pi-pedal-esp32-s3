#include "process_api.h"

#include <stdio.h>

#include "admin_auth.h"
#include "cJSON.h"
#include "local_process.h"
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
    if (!require_admin(r)) return ESP_OK;
    esp_err_t err = fn();
    if (err == ESP_OK) return send_status_json(r);
    prisma_process_status_t s = {0};
    (void)prisma_process_get_status(&s);
    if (err == ESP_ERR_NOT_FOUND) httpd_resp_set_status(r, "404 Not Found");
    else if (err == ESP_ERR_INVALID_STATE) httpd_resp_set_status(r, "409 Conflict");
    else if (err == ESP_ERR_NOT_SUPPORTED) httpd_resp_set_status(r, "415 Unsupported Media Type");
    else httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, s.error[0] ? s.error : fallback);
}

static esp_err_t poster_post(httpd_req_t *r){ return run_and_reply(r, prisma_process_run_poster, "poster processing failed"); }
static esp_err_t lines_post(httpd_req_t *r){ return run_and_reply(r, prisma_process_run_lines, "line generation failed"); }
static esp_err_t rectangles_post(httpd_req_t *r){ return run_and_reply(r, prisma_process_run_rectangles, "rectangle generation failed"); }
static esp_err_t circles_post(httpd_req_t *r){ return run_and_reply(r, prisma_process_run_circles, "circle generation failed"); }
static esp_err_t triangles_post(httpd_req_t *r){ return run_and_reply(r, prisma_process_run_triangles, "triangle generation failed"); }

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

static void register_one(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "route register failed %s method=%d: %s", uri->uri, (int)uri->method, esp_err_to_name(err));
}

void prisma_process_register_routes(httpd_handle_t server)
{
    (void)prisma_process_init();
    static const httpd_uri_t page = {.uri="/admin/process", .method=HTTP_GET, .handler=process_page_get};
    static const httpd_uri_t poster = {.uri="/api/v1/process/test/poster", .method=HTTP_POST, .handler=poster_post};
    static const httpd_uri_t lines = {.uri="/api/v1/process/test/lines", .method=HTTP_POST, .handler=lines_post};
    static const httpd_uri_t rectangles = {.uri="/api/v1/process/test/rectangles", .method=HTTP_POST, .handler=rectangles_post};
    static const httpd_uri_t circles = {.uri="/api/v1/process/test/circles", .method=HTTP_POST, .handler=circles_post};
    static const httpd_uri_t triangles = {.uri="/api/v1/process/test/triangles", .method=HTTP_POST, .handler=triangles_post};
    static const httpd_uri_t status = {.uri="/api/v1/process/test/status", .method=HTTP_GET, .handler=status_get};
    static const httpd_uri_t result = {.uri="/api/v1/process/test/result.bmp", .method=HTTP_GET, .handler=result_get};

    register_one(server, &page);
    register_one(server, &poster);
    register_one(server, &lines);
    register_one(server, &rectangles);
    register_one(server, &circles);
    register_one(server, &triangles);
    register_one(server, &status);
    register_one(server, &result);
    prisma_video_process_register_routes(server);
    ESP_LOGI(TAG, "CORE-4C local image + primitive + VIDEO processing ready at /admin/process");
}
