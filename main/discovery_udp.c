#include "discovery_udp.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define PRISMA_DISCOVERY_PORT 4210
#define PRISMA_DISCOVERY_MAGIC "PRISMA_DISCOVER_V1"

static const char *TAG = "PRISMA_UDP";
static bool s_task_started = false;

static void make_identity(char *device_id, size_t device_id_size,
                          char *hostname, size_t hostname_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strlcpy(device_id, "PRP-UNKNOWN", device_id_size);
        strlcpy(hostname, "prisma-pi-unknown", hostname_size);
        return;
    }

    snprintf(device_id, device_id_size, "PRP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(hostname, hostname_size, "prisma-pi-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void current_ip(char *out, size_t out_size)
{
    esp_netif_ip_info_t info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, out_size, IPSTR, IP2STR(&info.ip));
        return;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, out_size, IPSTR, IP2STR(&info.ip));
        return;
    }

    strlcpy(out, "0.0.0.0", out_size);
}

static void udp_discovery_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        s_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PRISMA_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed: errno=%d", PRISMA_DISCOVERY_PORT, errno);
        close(sock);
        s_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "READY udp/%d magic=%s", PRISMA_DISCOVERY_PORT, PRISMA_DISCOVERY_MAGIC);

    char rx[96];
    while (1) {
        struct sockaddr_in source_addr = {0};
        socklen_t source_len = sizeof(source_addr);
        int len = recvfrom(sock, rx, sizeof(rx) - 1, 0,
                           (struct sockaddr *)&source_addr, &source_len);
        if (len < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        rx[len] = '\0';
        if (strcmp(rx, PRISMA_DISCOVERY_MAGIC) != 0) continue;

        char device_id[20];
        char hostname[32];
        char ip[16];
        make_identity(device_id, sizeof(device_id), hostname, sizeof(hostname));
        current_ip(ip, sizeof(ip));

        char reply[256];
        int n = snprintf(reply, sizeof(reply),
            "{\"protocol\":\"PRISMA_DISCOVERY_V1\",\"device_id\":\"%s\",\"type\":\"pedal\",\"api\":\"v1\",\"ip\":\"%s\",\"hostname\":\"%s.local\",\"http_port\":80}",
            device_id, ip, hostname);
        if (n <= 0 || n >= (int)sizeof(reply)) continue;

        int sent = sendto(sock, reply, n, 0,
                          (struct sockaddr *)&source_addr, source_len);
        if (sent < 0) ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
    }
}

void prisma_udp_discovery_start_task(void)
{
    if (s_task_started) return;
    s_task_started = true;
    if (xTaskCreate(udp_discovery_task, "prisma_udp", 4096, NULL, 4, NULL) != pdPASS) {
        s_task_started = false;
        ESP_LOGE(TAG, "Could not start UDP discovery task");
    }
}
