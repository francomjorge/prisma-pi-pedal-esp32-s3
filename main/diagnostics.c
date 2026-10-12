#include "diagnostics.h"

#include <stdint.h>
#include <limits.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "PRISMA_DIAG";
static uint32_t s_boot_count = 0;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_UNKNOWN:
        default:                return "unknown";
    }
}

esp_err_t prisma_diagnostics_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("prisma_diag", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t count = 0;
    err = nvs_get_u32(h, "boot_count", &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        count = 0;
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        if (count < UINT32_MAX) count++;
        s_boot_count = count;
        err = nvs_set_u32(h, "boot_count", count);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot counter update failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "boot_count=%lu reset=%s", (unsigned long)s_boot_count,
             reset_reason_name(esp_reset_reason()));
    return ESP_OK;
}

uint32_t prisma_diagnostics_boot_count(void)
{
    return s_boot_count;
}

uint64_t prisma_diagnostics_uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

const char *prisma_diagnostics_reset_reason(void)
{
    return reset_reason_name(esp_reset_reason());
}

cJSON *prisma_diagnostics_system_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    cJSON_AddStringToObject(root, "target", "esp32s3");
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddNumberToObject(root, "revision", chip.revision);
    cJSON_AddStringToObject(root, "project", app ? app->project_name : "");
    cJSON_AddStringToObject(root, "firmware", app ? app->version : "");
    cJSON_AddStringToObject(root, "idf", app ? app->idf_ver : "");
    cJSON_AddStringToObject(root, "build_date", app ? app->date : "");
    cJSON_AddStringToObject(root, "build_time", app ? app->time : "");
    cJSON_AddNumberToObject(root, "boot_count", s_boot_count);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)prisma_diagnostics_uptime_ms());
    cJSON_AddStringToObject(root, "reset_reason", prisma_diagnostics_reset_reason());
    cJSON_AddNumberToObject(root, "reset_reason_code", esp_reset_reason());

    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    if (heap) {
        cJSON_AddNumberToObject(heap, "free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
        cJSON_AddNumberToObject(heap, "minimum_free", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        cJSON_AddNumberToObject(heap, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    return root;
}
