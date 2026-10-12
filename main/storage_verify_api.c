#include "storage_verify_api.h"

#include <stdio.h>

#include "admin_auth.h"
#include "image_ab.h"
#include "video_ab.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "PRISMA_VERIFY";

static void no_cache(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "Expires", "0");
}

static esp_err_t send_json(httpd_req_t *r, cJSON *root)
{
    if (!root) return ESP_ERR_NO_MEM;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    esp_err_t err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static void add_image_verify(cJSON *root)
{
    prisma_image_verify_t v = {0};
    esp_err_t err = prisma_image_ab_verify(&v);
    cJSON *j = cJSON_AddObjectToObject(root, "image");
    cJSON_AddBoolToObject(j, "ready", v.ready);
    cJSON_AddBoolToObject(j, "valid", v.valid);
    char active[2] = {v.active_bank ? v.active_bank : '-', 0};
    char inactive[2] = {v.inactive_bank ? v.inactive_bank : '-', 0};
    cJSON_AddStringToObject(j, "active_bank", active);
    cJSON_AddStringToObject(j, "inactive_bank", inactive);
    cJSON_AddNumberToObject(j, "generation", v.generation);
    cJSON_AddNumberToObject(j, "size", (double)v.size);
    char expected[9], actual[9];
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)v.expected_crc32);
    snprintf(actual, sizeof(actual), "%08lx", (unsigned long)v.actual_crc32);
    cJSON_AddStringToObject(j, "expected_crc32", expected);
    cJSON_AddStringToObject(j, "actual_crc32", actual);
    cJSON_AddBoolToObject(j, "crc_match", v.valid && err == ESP_OK && v.crc_match);
    cJSON_AddBoolToObject(j, "recovered_on_boot", v.recovered_on_boot);
    cJSON_AddStringToObject(j, "status", err == ESP_OK ? (v.valid ? (v.crc_match ? "verified" : "crc_mismatch") : "empty") : esp_err_to_name(err));
}

static void add_video_verify(cJSON *root)
{
    prisma_video_verify_t v = {0};
    esp_err_t err = prisma_video_ab_verify(&v);
    cJSON *j = cJSON_AddObjectToObject(root, "video");
    cJSON_AddBoolToObject(j, "ready", v.ready);
    cJSON_AddBoolToObject(j, "valid", v.valid);
    char active[2] = {v.active_bank ? v.active_bank : '-', 0};
    char inactive[2] = {v.inactive_bank ? v.inactive_bank : '-', 0};
    cJSON_AddStringToObject(j, "active_bank", active);
    cJSON_AddStringToObject(j, "inactive_bank", inactive);
    cJSON_AddNumberToObject(j, "generation", v.generation);
    cJSON_AddNumberToObject(j, "size", (double)v.size);
    char expected[9], actual[9];
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)v.expected_crc32);
    snprintf(actual, sizeof(actual), "%08lx", (unsigned long)v.actual_crc32);
    cJSON_AddStringToObject(j, "expected_crc32", expected);
    cJSON_AddStringToObject(j, "actual_crc32", actual);
    cJSON_AddBoolToObject(j, "crc_match", v.valid && err == ESP_OK && v.crc_match);
    cJSON_AddBoolToObject(j, "recovered_on_boot", v.recovered_on_boot);
    cJSON_AddStringToObject(j, "status", err == ESP_OK ? (v.valid ? (v.crc_match ? "verified" : "crc_mismatch") : "empty") : esp_err_to_name(err));
}

static esp_err_t verify_get(httpd_req_t *r)
{
    if (!prisma_admin_authorize(r)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scope", "admin");
    cJSON_AddStringToObject(root, "operation", "flash_readback_crc");
    add_image_verify(root);
    add_video_verify(root);
    return send_json(r, root);
}

void prisma_storage_verify_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t verify = {
        .uri = "/api/v1/admin/storage/verify",
        .method = HTTP_GET,
        .handler = verify_get
    };
    esp_err_t err = httpd_register_uri_handler(server, &verify);
    if (err != ESP_OK) ESP_LOGE(TAG, "verify route registration failed: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "storage readback verify ready");
}
