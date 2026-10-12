#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "prisma_display.h"
#include "prisma_power.h"
#include "prisma_storage.h"
#include "prisma_touch.h"

#define SWIPE_THRESHOLD 55
#define RELEASE_MISSES 4

static const char *TAG = "PRISMA_PEDAL";

void app_main(void)
{
    ESP_LOGI(TAG, "PRISMA-PI PEDAL CORE 0.1.0 / CORE-1");

    bool power_ok = prisma_power_hold_init();
    ESP_LOGI(TAG, "POWER: %s", power_ok ? "READY" : "FAILED");

    bool display_ok = prisma_display_init();
    ESP_LOGI(TAG, "DISPLAY: %s", display_ok ? "READY" : "FAILED");
    if (display_ok) prisma_display_boot_stage(0, power_ok);

    prisma_storage_status_t storage = {0};
    bool storage_ok = prisma_storage_init(&storage);
    ESP_LOGI(TAG, "STORAGE MAP: %s", storage_ok ? "READY" : "FAILED");
    if (display_ok) prisma_display_boot_stage(1, storage_ok);

    bool touch_ok = prisma_touch_init();
    ESP_LOGI(TAG, "TOUCH: %s", touch_ok ? "READY" : "FAILED");
    if (display_ok) prisma_display_boot_stage(2, touch_ok);

    bool core_ok = power_ok && display_ok && storage_ok && touch_ok;
    if (display_ok) prisma_display_boot_stage(3, core_ok);
    ESP_LOGI(TAG, "CORE-1: %s", core_ok ? "READY FOR HARDWARE TEST" : "DEGRADED");

    uint16_t x = 0, y = 0, start_x = 0, start_y = 0;
    bool down = false;
    int misses = 0;

    while (1) {
        bool touched = touch_ok && prisma_touch_read(&x, &y);
        if (touched) {
            misses = 0;
            if (!down) {
                start_x = x;
                start_y = y;
                down = true;
            }
        } else if (down && ++misses > RELEASE_MISSES) {
            int dx = (int)x - (int)start_x;
            int dy = (int)y - (int)start_y;
            if (abs(dx) >= SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                bool left = dx < 0;
                ESP_LOGI(TAG, "SWIPE %s start=(%u,%u) end=(%u,%u)",
                         left ? "LEFT" : "RIGHT", start_x, start_y, x, y);
                if (display_ok) prisma_display_swipe(left);
            } else {
                ESP_LOGI(TAG, "TOUCH (%u,%u)", start_x, start_y);
                if (display_ok) prisma_display_touch(start_x, start_y);
            }
            down = false;
            misses = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
