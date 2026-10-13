#include "network_ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "PRISMA_NET_UI";

static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

static void derive_hostname(char *dst, size_t size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(dst, size, "prisma-pi-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    } else {
        strlcpy(dst, "prisma-pi.local", size);
    }
}

static void derive_ap_ssid(char *dst, size_t size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(dst, size, "PRISMA-PI-%02X%02X", mac[4], mac[5]);
    } else {
        strlcpy(dst, "PRISMA-PI-SETUP", size);
    }
}

static void get_ip_for_key(const char *ifkey, char *dst, size_t size)
{
    if (!dst || size == 0) return;
    dst[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t info = {0};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(dst, size, IPSTR, IP2STR(&info.ip));
    }
}

esp_err_t prisma_network_snapshot(prisma_network_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    derive_hostname(out->hostname, sizeof(out->hostname));

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        strlcpy(out->mode, "STARTING", sizeof(out->mode));
        return ESP_OK;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->connected = true;
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        get_ip_for_key("WIFI_STA_DEF", out->ip, sizeof(out->ip));
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t ap_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
            strlcpy(out->ap_ssid, (const char *)ap_cfg.ap.ssid, sizeof(out->ap_ssid));
        }
        if (!out->connected) {
            out->provisioning = true;
            strlcpy(out->ssid, out->ap_ssid[0] ? out->ap_ssid : "PRISMA-PI", sizeof(out->ssid));
            get_ip_for_key("WIFI_AP_DEF", out->ip, sizeof(out->ip));
            if (!out->ip[0]) strlcpy(out->ip, "192.168.4.1", sizeof(out->ip));
        }
    }

    if (out->connected) strlcpy(out->mode, "CONNECTED", sizeof(out->mode));
    else if (out->provisioning) strlcpy(out->mode, "AP SETUP", sizeof(out->mode));
    else strlcpy(out->mode, "OFFLINE", sizeof(out->mode));

    return ESP_OK;
}

esp_err_t prisma_network_ensure_fallback_ap(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    if (mode == WIFI_MODE_NULL) return ESP_ERR_INVALID_STATE;

    char ssid[33];
    derive_ap_ssid(ssid, sizeof(ssid));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    if (mode == WIFI_MODE_STA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Fallback AP armed: %s / http://192.168.4.1", ssid);
    }
    return err;
}

static bool candidate_has_ip(const char *ssid, char *ip, size_t ip_size)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (strcmp((const char *)ap.ssid, ssid) != 0) return false;

    char current_ip[16] = {0};
    get_ip_for_key("WIFI_STA_DEF", current_ip, sizeof(current_ip));
    if (!current_ip[0]) return false;
    if (ip && ip_size) strlcpy(ip, current_ip, ip_size);
    return true;
}

esp_err_t prisma_network_test_wifi(const char *ssid, const char *password,
                                   prisma_wifi_test_result_t *out)
{
    if (!ssid || !ssid[0] || !password || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err = prisma_network_ensure_fallback_ap();
    if (err != ESP_OK) return err;

    wifi_config_t old_sta = {0};
    bool have_old_config = esp_wifi_get_config(WIFI_IF_STA, &old_sta) == ESP_OK && old_sta.sta.ssid[0] != 0;
    wifi_ap_record_t old_ap = {0};
    bool was_connected = esp_wifi_sta_get_ap_info(&old_ap) == ESP_OK;

    wifi_config_t candidate = {0};
    strlcpy((char *)candidate.sta.ssid, ssid, sizeof(candidate.sta.ssid));
    strlcpy((char *)candidate.sta.password, password, sizeof(candidate.sta.password));
    candidate.sta.threshold.authmode = WIFI_AUTH_OPEN;
    candidate.sta.pmf_cfg.capable = true;
    candidate.sta.pmf_cfg.required = false;

    /* Our prisma_net NVS is the source of truth. Do not persist an unvalidated
       candidate in the Wi-Fi driver's own storage. */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (err != ESP_OK) return err;

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(80));
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;

    for (int i = 0; i < 60; ++i) {
        if (candidate_has_ip(ssid, out->ip, sizeof(out->ip))) {
            out->connected = true;
            ESP_LOGI(TAG, "Wi-Fi validation OK ssid=%s ip=%s", ssid, out->ip);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    /* Validation failed: never commit or reboot. Restore the previous STA
       connection when one existed, while the PRISMA fallback AP stays alive. */
    out->connected = false;
    (void)esp_wifi_disconnect();
    if (have_old_config) {
        (void)esp_wifi_set_config(WIFI_IF_STA, &old_sta);
        if (was_connected) (void)esp_wifi_connect();
    }
    ESP_LOGW(TAG, "Wi-Fi validation failed ssid=%s; credentials not committed", ssid);
    return ESP_OK;
}

void prisma_network_restart(void)
{
    if (xTaskCreate(delayed_restart, "ui_restart", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create restart task");
    }
}

esp_err_t prisma_network_forget_and_restart(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("prisma_net", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        prisma_network_restart();
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG, "Saved Wi-Fi forgotten from appliance UI");
    prisma_network_restart();
    return ESP_OK;
}
