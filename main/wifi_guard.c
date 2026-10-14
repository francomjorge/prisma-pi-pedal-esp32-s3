#include "wifi_guard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "video_ab.h"

static const char *TAG = "PRISMA_WIFI_GUARD";
static bool s_initialized = false;

esp_err_t __real_httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);

static void no_cache(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "Expires", "0");
}

static esp_err_t send_json_text(httpd_req_t *r, const char *status, const char *text)
{
    if (status) httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    return httpd_resp_sendstr(r, text);
}

static esp_err_t read_body(httpd_req_t *r, char *dst, size_t cap)
{
    if (!r || !dst || cap < 2 || r->content_len <= 0 || (size_t)r->content_len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t off = 0;
    while (off < (size_t)r->content_len) {
        int got = httpd_req_recv(r, dst + off, (size_t)r->content_len - off);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        off += (size_t)got;
    }
    dst[off] = '\0';
    return ESP_OK;
}

static esp_err_t downloadable_video_get(httpd_req_t *r)
{
    prisma_video_info_t info = {0};
    esp_err_t err = prisma_video_ab_get_info(&info);
    if (err != ESP_OK || !info.valid || !info.size) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "No active VIDEO");
        return ESP_FAIL;
    }

    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"PRISMA_VIDEO_ACTIVE.bin\"");
    no_cache(r);

    char size_text[24];
    snprintf(size_text, sizeof(size_text), "%u", (unsigned)info.size);
    httpd_resp_set_hdr(r, "X-Prisma-Source-Bytes", size_text);
    char crc_text[16];
    snprintf(crc_text, sizeof(crc_text), "%08lx", (unsigned long)info.crc32);
    httpd_resp_set_hdr(r, "X-Prisma-CRC32", crc_text);

    uint8_t buf[1024];
    size_t offset = 0;
    while (offset < info.size) {
        size_t take = info.size - offset;
        if (take > sizeof(buf)) take = sizeof(buf);
        err = prisma_video_ab_read(offset, buf, take);
        if (err != ESP_OK) {
            (void)httpd_resp_send_chunk(r, NULL, 0);
            return err;
        }
        err = httpd_resp_send_chunk(r, (const char *)buf, take);
        if (err != ESP_OK) return err;
        offset += take;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t save_credentials(const char *ssid, const char *password)
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

static void fallback_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    uint8_t mac[6] = {0};
    char ssid[32] = "PRISMA-PI-SETUP";
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "PRISMA-PI-%02X%02X", mac[4], mac[5]);
    }
    strlcpy((char *)cfg->ap.ssid, ssid, sizeof(cfg->ap.ssid));
    cfg->ap.ssid_len = strlen(ssid);
    cfg->ap.max_connection = 4;
    cfg->ap.authmode = WIFI_AUTH_OPEN;
}

static esp_err_t ensure_fallback_ap(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;

    if (mode == WIFI_MODE_STA) {
        wifi_config_t ap = {0};
        fallback_ap_config(&ap);
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
        return esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (mode == WIFI_MODE_AP) {
        return esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    return ESP_OK;
}

static bool wait_candidate_connected(const char *ssid, char *ip_text, size_t ip_size)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 60; ++i) {
        wifi_ap_record_t ap = {0};
        esp_netif_ip_info_t ip = {0};
        bool ap_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        bool same = ap_ok && strcmp((const char *)ap.ssid, ssid) == 0;
        bool ip_ok = sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
        if (same && ip_ok) {
            snprintf(ip_text, ip_size, IPSTR, IP2STR(&ip.ip));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ip_size) ip_text[0] = '\0';
    return false;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

static esp_err_t guarded_connect_post(httpd_req_t *r)
{
    char body[256];
    if (read_body(r, body, sizeof(body)) != ESP_OK) {
        return send_json_text(r, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid_body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_json_text(r, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid_json\"}");
    cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *ssid = cJSON_IsString(ssid_j) ? ssid_j->valuestring : NULL;
    const char *password = cJSON_IsString(pass_j) ? pass_j->valuestring : "";
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 63) {
        cJSON_Delete(root);
        return send_json_text(r, "400 Bad Request", "{\"ok\":false,\"error\":\"ssid_or_password_length\"}");
    }

    char candidate_ssid[33];
    char candidate_password[64];
    strlcpy(candidate_ssid, ssid, sizeof(candidate_ssid));
    strlcpy(candidate_password, password, sizeof(candidate_password));
    cJSON_Delete(root);

    wifi_mode_t previous_mode = WIFI_MODE_NULL;
    wifi_config_t previous_sta = {0};
    wifi_ap_record_t previous_ap = {0};
    bool previous_connected = esp_wifi_sta_get_ap_info(&previous_ap) == ESP_OK;
    (void)esp_wifi_get_mode(&previous_mode);
    bool have_previous_cfg = esp_wifi_get_config(WIFI_IF_STA, &previous_sta) == ESP_OK && previous_sta.sta.ssid[0] != '\0';

    ESP_LOGI(TAG, "Testing Wi-Fi candidate SSID=%s before persistence", candidate_ssid);
    esp_err_t err = ensure_fallback_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable fallback AP: %s", esp_err_to_name(err));
        return send_json_text(r, "500 Internal Server Error", "{\"ok\":false,\"error\":\"fallback_ap_failed\"}");
    }

    wifi_config_t candidate = {0};
    strlcpy((char *)candidate.sta.ssid, candidate_ssid, sizeof(candidate.sta.ssid));
    strlcpy((char *)candidate.sta.password, candidate_password, sizeof(candidate.sta.password));
    candidate.sta.threshold.authmode = WIFI_AUTH_OPEN;
    candidate.sta.pmf_cfg.capable = true;
    candidate.sta.pmf_cfg.required = false;

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));
    err = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (err == ESP_OK) err = esp_wifi_connect();

    char ip[16] = "";
    bool validated = err == ESP_OK && wait_candidate_connected(candidate_ssid, ip, sizeof(ip));
    if (!validated) {
        ESP_LOGW(TAG, "Candidate Wi-Fi rejected/unreachable SSID=%s; credentials NOT saved", candidate_ssid);
        (void)esp_wifi_disconnect();
        if (have_previous_cfg) {
            (void)esp_wifi_set_config(WIFI_IF_STA, &previous_sta);
            if (previous_connected) (void)esp_wifi_connect();
        }
        if (previous_mode == WIFI_MODE_AP && !previous_connected) {
            (void)esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return send_json_text(r, "422 Unprocessable Entity",
                              "{\"ok\":false,\"validated\":false,\"saved\":false,\"restarting\":false,\"error\":\"wifi_auth_or_network_failed\"}");
    }

    err = save_credentials(candidate_ssid, candidate_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Candidate connected but NVS save failed: %s", esp_err_to_name(err));
        return send_json_text(r, "500 Internal Server Error",
                              "{\"ok\":false,\"validated\":true,\"saved\":false,\"restarting\":false,\"error\":\"nvs_save_failed\"}");
    }

    char response[192];
    int n = snprintf(response, sizeof(response),
                     "{\"ok\":true,\"validated\":true,\"saved\":true,\"restarting\":true,\"ssid\":\"%s\",\"ip\":\"%s\"}",
                     candidate_ssid, ip);
    if (n < 0 || n >= (int)sizeof(response)) {
        return send_json_text(r, "500 Internal Server Error", "{\"ok\":false,\"error\":\"response_overflow\"}");
    }
    ESP_LOGI(TAG, "Candidate Wi-Fi validated SSID=%s ip=%s; saving and restarting", candidate_ssid, ip);
    esp_err_t response_err = send_json_text(r, NULL, response);
    xTaskCreate(restart_task, "wifi_valid_restart", 2048, NULL, 5, NULL);
    return response_err;
}

esp_err_t prisma_wifi_guard_init(void)
{
    if (s_initialized) return ESP_OK;
    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi guarded provisioning armed");
    return ESP_OK;
}

esp_err_t __wrap_httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    if (!uri_handler) return ESP_ERR_INVALID_ARG;
    if (uri_handler->method == HTTP_POST && uri_handler->uri &&
        strcmp(uri_handler->uri, "/api/v1/wifi/connect") == 0) {
        httpd_uri_t guarded = *uri_handler;
        guarded.handler = guarded_connect_post;
        (void)prisma_wifi_guard_init();
        ESP_LOGI(TAG, "Replacing legacy SAVE-first Wi-Fi handler with validate-first handler");
        return __real_httpd_register_uri_handler(handle, &guarded);
    }
    if (uri_handler->method == HTTP_GET && uri_handler->uri &&
        strcmp(uri_handler->uri, "/api/v1/video") == 0) {
        httpd_uri_t download = *uri_handler;
        download.handler = downloadable_video_get;
        ESP_LOGI(TAG, "VIDEO GET armed as downloadable PRISMA_VIDEO_ACTIVE.bin");
        return __real_httpd_register_uri_handler(handle, &download);
    }
    return __real_httpd_register_uri_handler(handle, uri_handler);
}
