#include <stdio.h>
#include <string.h>

#include "admin_api.h"
#include "admin_auth.h"
#include "network_ui.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "PRISMA_WIFI_HARDEN";

/* admin_api.c is compiled with its exported register symbol renamed so we can
   keep every existing CORE-5A route and replace only Wi-Fi connect. */
void prisma_admin_register_routes_legacy(httpd_handle_t server);

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
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    esp_err_t err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    return err;
}

static esp_err_t recv_json(httpd_req_t *r, char *buf, size_t capacity, cJSON **out)
{
    if (!buf || !out || capacity < 2) return ESP_ERR_INVALID_ARG;
    if (r->content_len <= 0 || (size_t)r->content_len >= capacity) return ESP_ERR_INVALID_SIZE;

    size_t offset = 0;
    while (offset < (size_t)r->content_len) {
        int got = httpd_req_recv(r, buf + offset, (size_t)r->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        offset += (size_t)got;
    }
    buf[offset] = '\0';
    *out = cJSON_Parse(buf);
    return *out ? ESP_OK : ESP_ERR_INVALID_ARG;
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

static esp_err_t wifi_connect_validated_post(httpd_req_t *r)
{
    if (!prisma_admin_authorize(r)) return ESP_OK;

    char body[256];
    cJSON *root = NULL;
    if (recv_json(r, body, sizeof(body), &root) != ESP_OK) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *ssid_text = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_text = cJSON_IsString(password) ? password->valuestring : "";
    size_t ssid_len = ssid_text ? strlen(ssid_text) : 0;
    size_t pass_len = strlen(pass_text);

    if (ssid_len == 0 || ssid_len > 32 || pass_len > 63) {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "SSID/password length invalid");
    }

    char ssid_copy[33];
    char pass_copy[64];
    strlcpy(ssid_copy, ssid_text, sizeof(ssid_copy));
    strlcpy(pass_copy, pass_text, sizeof(pass_copy));
    cJSON_Delete(root);

    prisma_wifi_test_result_t test = {0};
    esp_err_t err = prisma_network_test_wifi(ssid_copy, pass_copy, &test);

    if (err != ESP_OK) {
        memset(pass_copy, 0, sizeof(pass_copy));
        ESP_LOGE(TAG, "Wi-Fi validation internal error: %s", esp_err_to_name(err));
        httpd_resp_set_status(r, "503 Service Unavailable");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddBoolToObject(j, "validated", false);
        cJSON_AddStringToObject(j, "message", "Wi-Fi validation could not run");
        return send_json(r, j);
    }

    if (!test.connected) {
        memset(pass_copy, 0, sizeof(pass_copy));
        ESP_LOGW(TAG, "Rejected unvalidated Wi-Fi ssid=%s", ssid_copy);
        httpd_resp_set_status(r, "422 Unprocessable Entity");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddBoolToObject(j, "validated", false);
        cJSON_AddBoolToObject(j, "restarting", false);
        cJSON_AddStringToObject(j, "ssid", ssid_copy);
        cJSON_AddStringToObject(j, "message", "Password or network validation failed; settings were not saved");
        return send_json(r, j);
    }

    /* The password has already been used successfully to associate and obtain
       DHCP. Only now make it persistent. */
    err = save_wifi_credentials(ssid_copy, pass_copy);
    memset(pass_copy, 0, sizeof(pass_copy));
    if (err != ESP_OK) {
        httpd_resp_set_status(r, "500 Internal Server Error");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddBoolToObject(j, "validated", true);
        cJSON_AddStringToObject(j, "message", "Connected, but credentials could not be saved");
        return send_json(r, j);
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "validated", true);
    cJSON_AddBoolToObject(j, "restarting", true);
    cJSON_AddStringToObject(j, "ssid", ssid_copy);
    cJSON_AddStringToObject(j, "ip", test.ip);
    cJSON_AddStringToObject(j, "fallback_ap", "192.168.4.1");
    esp_err_t response = send_json(r, j);
    prisma_network_restart();
    return response;
}

void prisma_admin_register_routes(httpd_handle_t server)
{
    prisma_admin_register_routes_legacy(server);

    /* Replace the old save-then-reboot handler with test -> DHCP -> save -> reboot. */
    (void)httpd_unregister_uri_handler(server, "/api/v1/wifi/connect", HTTP_POST);
    static const httpd_uri_t connect = {
        .uri = "/api/v1/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_validated_post
    };
    esp_err_t err = httpd_register_uri_handler(server, &connect);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "validated Wi-Fi route failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Validated Wi-Fi connect route armed");
    }
}
