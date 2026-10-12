#include "discovery.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"

static const char *TAG = "PRISMA_DISCOVERY";
static bool s_task_started = false;

static void discovery_task(void *arg)
{
    (void)arg;

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *sta = NULL;

    while (1) {
        sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read STA MAC");
        vTaskDelete(NULL);
        return;
    }

    char hostname[32];
    char instance[64];
    char device_id[20];
    snprintf(hostname, sizeof(hostname), "prisma-pi-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(instance, sizeof(instance), "PRISMA-Pi Pedal %02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(device_id, sizeof(device_id), "PRP-%02X%02X%02X", mac[3], mac[4], mac[5]);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    if ((err = mdns_hostname_set(hostname)) != ESP_OK ||
        (err = mdns_instance_name_set(instance)) != ESP_OK) {
        ESP_LOGE(TAG, "mDNS identity failed: %s", esp_err_to_name(err));
        mdns_free();
        vTaskDelete(NULL);
        return;
    }

    mdns_txt_item_t prisma_txt[] = {
        {"id", device_id},
        {"type", "pedal"},
        {"api", "v1"}
    };

    err = mdns_service_add(NULL, "_prisma", "_tcp", 80, prisma_txt, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "_prisma service failed: %s", esp_err_to_name(err));
        mdns_free();
        vTaskDelete(NULL);
        return;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "_http service not published: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "READY http://%s.local  service=_prisma._tcp id=%s", hostname, device_id);
    vTaskDelete(NULL);
}

void prisma_discovery_start_task(void)
{
    if (s_task_started) return;
    s_task_started = true;
    if (xTaskCreate(discovery_task, "prisma_mdns", 4096, NULL, 4, NULL) != pdPASS) {
        s_task_started = false;
        ESP_LOGE(TAG, "Could not start discovery task");
    }
}
