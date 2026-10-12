#include "prisma_power.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BAT_POWER_PIN GPIO_NUM_2

static const char *TAG = "PEDAL_POWER";

bool prisma_power_hold_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BAT_POWER_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    if (gpio_set_level(BAT_POWER_PIN, 1) != ESP_OK) return false;
    ESP_LOGI(TAG, "power hold asserted on GPIO%d", BAT_POWER_PIN);
    return true;
}

void prisma_power_release(void)
{
    ESP_LOGW(TAG, "power hold released");
    gpio_set_level(BAT_POWER_PIN, 0);
}
