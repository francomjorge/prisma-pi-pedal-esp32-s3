#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "admin_api.h"
#include "admin_auth.h"
#include "image_ab.h"
#include "video_ab.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#define WIFI_SCAN_LIMIT 20
#define LEGACY_LIST_LIMIT 48

static const char *TAG = "PRISMA_ADMIN";

extern const uint8_t web_admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t web_admin_html_end[] asm("_binary_admin_html_end");

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

static bool require_admin(httpd_req_t *r)
{
    return prisma_admin_authorize(r);
}

static esp_err_t admin_page_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    no_cache(r);
    return httpd_resp_send(r, (const char *)web_admin_html_start,
                           (ssize_t)(web_admin_html_end - web_admin_html_start));
}

static esp_err_t storage_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scope", "admin");

    prisma_image_info_t image = {0};
    prisma_video_info_t video = {0};
    bool image_ok = prisma_image_ab_get_info(&image) == ESP_OK;
    bool video_ok = prisma_video_ab_get_info(&video) == ESP_OK;

    cJSON *ij = cJSON_AddObjectToObject(root, "image");
    cJSON_AddBoolToObject(ij, "ready", image_ok && image.ready);
    cJSON_AddBoolToObject(ij, "valid", image_ok && image.valid);
    char ibank[2] = { image_ok ? image.active_bank : '-', 0 };
    cJSON_AddStringToObject(ij, "active_bank", ibank);
    cJSON_AddNumberToObject(ij, "generation", image_ok ? image.generation : 0);
    cJSON_AddNumberToObject(ij, "size", image_ok ? image.size : 0);
    cJSON_AddNumberToObject(ij, "capacity", PRISMA_IMAGE_MAX_BYTES);
    char icrc[9];
    snprintf(icrc, sizeof(icrc), "%08lx", (unsigned long)(image_ok ? image.crc32 : 0));
    cJSON_AddStringToObject(ij, "crc32", icrc);

    cJSON *vj = cJSON_AddObjectToObject(root, "video");
    cJSON_AddBoolToObject(vj, "ready", video_ok && video.ready);
    cJSON_AddBoolToObject(vj, "valid", video_ok && video.valid);
    char vbank[2] = { video_ok ? video.active_bank : '-', 0 };
    cJSON_AddStringToObject(vj, "active_bank", vbank);
    cJSON_AddNumberToObject(vj, "generation", video_ok ? video.generation : 0);
    cJSON_AddNumberToObject(vj, "size", video_ok ? video.size : 0);
    cJSON_AddNumberToObject(vj, "capacity", PRISMA_VIDEO_MAX_BYTES);
    char vcrc[9];
    snprintf(vcrc, sizeof(vcrc), "%08lx", (unsigned long)(video_ok ? video.crc32 : 0));
    cJSON_AddStringToObject(vj, "crc32", vcrc);

    cJSON *legacy = cJSON_AddObjectToObject(root, "legacy_spiffs");
    size_t total = 0, used = 0;
    bool mounted = esp_spiffs_info("storage", &total, &used) == ESP_OK;
    cJSON_AddBoolToObject(legacy, "mounted", mounted);
    cJSON_AddNumberToObject(legacy, "total", total);
    cJSON_AddNumberToObject(legacy, "used", used);
    cJSON_AddNumberToObject(legacy, "free", total >= used ? total - used : 0);

    cJSON *files = cJSON_AddArrayToObject(legacy, "files");
    DIR *dir = opendir("/storage");
    int listed = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && listed < LEGACY_LIST_LIMIT) {
            if (!entry->d_name[0] || entry->d_name[0] == '.') continue;
            char path[96];
            int n = snprintf(path, sizeof(path), "/storage/%s", entry->d_name);
            if (n <= 0 || n >= (int)sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", entry->d_name);
            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
            cJSON_AddItemToArray(files, f);
            listed++;
        }
        closedir(dir);
    }
    cJSON_AddNumberToObject(legacy, "file_count", listed);
    return send_json(r, root);
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("prisma_net", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "password", password ? password : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t clear_wifi_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("prisma_net", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

static esp_err_t wifi_scan_get(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;

    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "Wi-Fi scan unavailable");
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) count = 0;
    if (count > WIFI_SCAN_LIMIT) count = WIFI_SCAN_LIMIT;
    wifi_ap_record_t *records = count ? calloc(count, sizeof(*records)) : NULL;
    if (count && !records) return ESP_ERR_NO_MEM;
    uint16_t found = count;
    if (count && esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) found = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < found; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddItemToArray(items, item);
    }
    free(records);
    return send_json(r, root);
}

static esp_err_t wifi_connect_post(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    if (r->content_len <= 0 || r->content_len > 255) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        return ESP_FAIL;
    }

    char body[256];
    size_t offset = 0;
    while (offset < (size_t)r->content_len) {
        int got = httpd_req_recv(r, body + offset, (size_t)r->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        offset += (size_t)got;
    }
    body[offset] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *ssid_text = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *password_text = cJSON_IsString(password) ? password->valuestring : "";
    size_t ssid_len = ssid_text ? strlen(ssid_text) : 0;
    size_t pass_len = strlen(password_text);
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 63) {
        cJSON_Delete(root);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "SSID/password length invalid");
        return ESP_FAIL;
    }

    esp_err_t err = save_wifi_credentials(ssid_text, password_text);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi");
        return err;
    }

    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    esp_err_t response = httpd_resp_sendstr(r, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "admin_wifi_restart", 2048, NULL, 5, NULL);
    return response;
}

static esp_err_t wifi_reset_post(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    esp_err_t err = clear_wifi_credentials();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not reset Wi-Fi");
        return err;
    }
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    esp_err_t response = httpd_resp_sendstr(r, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "admin_wifi_reset", 2048, NULL, 5, NULL);
    return response;
}

static esp_err_t admin_password_post(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    if (r->content_len <= 0 || r->content_len > 255) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        return ESP_FAIL;
    }

    char body[256];
    size_t offset = 0;
    while (offset < (size_t)r->content_len) {
        int got = httpd_req_recv(r, body + offset, (size_t)r->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        offset += (size_t)got;
    }
    body[offset] = '\0';

    cJSON *root = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (!root) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *new_password = cJSON_GetObjectItemCaseSensitive(root, "new_password");
    cJSON *confirm_password = cJSON_GetObjectItemCaseSensitive(root, "confirm_password");
    const char *new_text = cJSON_IsString(new_password) ? new_password->valuestring : NULL;
    const char *confirm_text = cJSON_IsString(confirm_password) ? confirm_password->valuestring : NULL;
    size_t pass_len = new_text ? strlen(new_text) : 0;

    if (!new_text || !confirm_text || pass_len < 8 || pass_len > 64 || strcmp(new_text, confirm_text) != 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Password must match and be 8-64 characters");
        return ESP_FAIL;
    }

    esp_err_t err = prisma_admin_set_password(new_text);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save admin password");
        return err;
    }

    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    return httpd_resp_sendstr(r, "{\"ok\":true,\"credentials_changed\":true}");
}

static esp_err_t image_put(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    size_t total = (size_t)r->content_len;
    if (total < 4 || total > PRISMA_IMAGE_MAX_BYTES) {
        httpd_resp_set_status(r, "413 Payload Too Large");
        return httpd_resp_sendstr(r, "IMAGE must be JPEG <= 1 MB");
    }

    esp_err_t err = prisma_image_ab_begin(total);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start IMAGE upload");
        return err;
    }

    uint8_t buf[1024];
    size_t offset = 0;
    uint8_t first0 = 0, first1 = 0, last0 = 0, last1 = 0;
    while (offset < total) {
        size_t want = total - offset;
        if (want > sizeof(buf)) want = sizeof(buf);
        int got = httpd_req_recv(r, (char *)buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            prisma_image_ab_abort();
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "IMAGE upload interrupted");
            return ESP_FAIL;
        }
        for (int i = 0; i < got; ++i) {
            size_t pos = offset + (size_t)i;
            if (pos == 0) first0 = buf[i];
            if (pos == 1) first1 = buf[i];
            last0 = last1;
            last1 = buf[i];
        }
        err = prisma_image_ab_write(offset, buf, (size_t)got);
        if (err != ESP_OK) {
            prisma_image_ab_abort();
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "IMAGE flash write failed");
            return err;
        }
        offset += (size_t)got;
    }

    if (first0 != 0xFF || first1 != 0xD8 || last0 != 0xFF || last1 != 0xD9) {
        prisma_image_ab_abort();
        httpd_resp_set_status(r, "415 Unsupported Media Type");
        return httpd_resp_sendstr(r, "Invalid JPEG; previous IMAGE remains active");
    }

    err = prisma_image_ab_finalize();
    if (err != ESP_OK) {
        prisma_image_ab_abort();
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "IMAGE commit failed");
        return err;
    }

    prisma_image_info_t info = {0};
    prisma_image_ab_get_info(&info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char bank[2] = {info.active_bank, 0};
    cJSON_AddStringToObject(root, "active_bank", bank);
    cJSON_AddNumberToObject(root, "generation", info.generation);
    cJSON_AddNumberToObject(root, "size", info.size);
    return send_json(r, root);
}

static esp_err_t image_delete(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    esp_err_t err = prisma_image_ab_delete();
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "IMAGE delete failed");
        return err;
    }
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    return httpd_resp_sendstr(r, "{\"ok\":true,\"valid\":false}");
}

static esp_err_t video_put(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    size_t total = (size_t)r->content_len;
    if (total == 0 || total > PRISMA_VIDEO_MAX_BYTES) {
        httpd_resp_set_status(r, "413 Payload Too Large");
        return httpd_resp_sendstr(r, "VIDEO payload must be <= 3 MB");
    }

    esp_err_t err = prisma_video_ab_begin(total);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start VIDEO upload");
        return err;
    }

    uint8_t buf[1024];
    size_t offset = 0;
    while (offset < total) {
        size_t want = total - offset;
        if (want > sizeof(buf)) want = sizeof(buf);
        int got = httpd_req_recv(r, (char *)buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            prisma_video_ab_abort();
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "VIDEO upload interrupted");
            return ESP_FAIL;
        }
        err = prisma_video_ab_write(offset, buf, (size_t)got);
        if (err != ESP_OK) {
            prisma_video_ab_abort();
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "VIDEO flash write failed");
            return err;
        }
        offset += (size_t)got;
    }

    err = prisma_video_ab_finalize();
    if (err != ESP_OK) {
        prisma_video_ab_abort();
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "VIDEO commit failed");
        return err;
    }

    prisma_video_info_t info = {0};
    prisma_video_ab_get_info(&info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char bank[2] = {info.active_bank, 0};
    cJSON_AddStringToObject(root, "active_bank", bank);
    cJSON_AddNumberToObject(root, "generation", info.generation);
    cJSON_AddNumberToObject(root, "size", info.size);
    return send_json(r, root);
}

static esp_err_t video_delete(httpd_req_t *r)
{
    if (!require_admin(r)) return ESP_OK;
    esp_err_t err = prisma_video_ab_delete();
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "VIDEO delete failed");
        return err;
    }
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    return httpd_resp_sendstr(r, "{\"ok\":true,\"valid\":false}");
}

static void register_one(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route register failed %s method=%d: %s",
                 uri->uri, (int)uri->method, esp_err_to_name(err));
    }
}

void prisma_admin_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t page = {
        .uri = "/admin", .method = HTTP_GET, .handler = admin_page_get
    };
    static const httpd_uri_t storage = {
        .uri = "/api/v1/admin/storage", .method = HTTP_GET, .handler = storage_get
    };
    static const httpd_uri_t password = {
        .uri = "/api/v1/admin/password", .method = HTTP_POST, .handler = admin_password_post
    };
    static const httpd_uri_t scan = {
        .uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get
    };
    static const httpd_uri_t connect = {
        .uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post
    };
    static const httpd_uri_t reset = {
        .uri = "/api/v1/wifi/reset", .method = HTTP_POST, .handler = wifi_reset_post
    };
    static const httpd_uri_t image_upload = {
        .uri = "/api/v1/image", .method = HTTP_PUT, .handler = image_put
    };
    static const httpd_uri_t image_remove = {
        .uri = "/api/v1/image", .method = HTTP_DELETE, .handler = image_delete
    };
    static const httpd_uri_t video_upload = {
        .uri = "/api/v1/video", .method = HTTP_PUT, .handler = video_put
    };
    static const httpd_uri_t video_remove = {
        .uri = "/api/v1/video", .method = HTTP_DELETE, .handler = video_delete
    };

    register_one(server, &page);
    register_one(server, &storage);
    register_one(server, &password);
    register_one(server, &scan);
    register_one(server, &connect);
    register_one(server, &reset);
    register_one(server, &image_upload);
    register_one(server, &image_remove);
    register_one(server, &video_upload);
    register_one(server, &video_remove);

    ESP_LOGI(TAG, "CORE-4F admin routes ready at /admin");
}
