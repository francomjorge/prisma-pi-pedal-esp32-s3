#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool connected;
    bool provisioning;
    char mode[16];
    char ssid[33];
    char ip[16];
    char hostname[32];
    char ap_ssid[33];
} prisma_network_snapshot_t;

typedef struct {
    bool connected;
    int disconnect_reason;
    char ip[16];
} prisma_wifi_test_result_t;

esp_err_t prisma_network_snapshot(prisma_network_snapshot_t *out);
esp_err_t prisma_network_ensure_fallback_ap(void);
esp_err_t prisma_network_test_wifi(const char *ssid, const char *password,
                                   prisma_wifi_test_result_t *out);
void prisma_network_restart(void);
esp_err_t prisma_network_forget_and_restart(void);
