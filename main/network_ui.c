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
    vTaskDelay(pdMS_TO_TICKS(500));
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
