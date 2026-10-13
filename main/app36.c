#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "image_ab.h"
#include "video_ab.h"
#include "processed_store.h"
#include "discovery.h"
#include "discovery_udp.h"

#define BAT_POWER_PIN 2

#define synth_on prisma_shared_on
#define mode_index prisma_shared_mode
#define intensity prisma_shared_intensity
#define app_main prisma_legacy_main

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "main.c"
#pragma GCC diagnostic pop

#undef app_main
#undef intensity
#undef mode_index
#undef synth_on

static const char *SYNC_TAG = "WEB_SYNC";

static void prisma_early_power_hold(void)
{
    gpio_config_t p = {.pin_bit_mask = 1ULL << BAT_POWER_PIN,.mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&p));
    ESP_ERROR_CHECK(gpio_set_level(BAT_POWER_PIN, 1));
}

bool prisma_appliance_get_on(void){return prisma_shared_on;}
int prisma_appliance_get_mode(void){return prisma_shared_mode ? 1 : 0;}
int prisma_appliance_get_intensity(void){return prisma_shared_intensity;}
const char *prisma_appliance_get_filename(void){return "IMAGE-A/B";}
bool prisma_appliance_get_effect(int effect){if(effect<0||effect>=EFFECT_COUNT)return false;if(effect==5)return true;return effect_enabled[effect];}
int prisma_appliance_get_effect_value(int effect){if(effect<0||effect>=EFFECT_COUNT)return 0;return effect_value[effect];}

void app_main(void)
{
    prisma_early_power_hold();
    prisma_shared_mode = 0;
    prisma_shared_intensity = 61;

    esp_err_t image_ab = prisma_image_ab_init();
    if (image_ab != ESP_OK) ESP_LOGW(SYNC_TAG, "IMAGE A/B unavailable: %s", esp_err_to_name(image_ab));
    esp_err_t video_ab = prisma_video_ab_init();
    if (video_ab != ESP_OK) ESP_LOGW(SYNC_TAG, "VIDEO A/B unavailable: %s", esp_err_to_name(video_ab));
    esp_err_t outputs = prisma_processed_store_init();
    if (outputs != ESP_OK) ESP_LOGW(SYNC_TAG, "Processed output store unavailable: %s", esp_err_to_name(outputs));

    prisma_discovery_start_task();
    prisma_udp_discovery_start_task();
    ESP_LOGI(SYNC_TAG, "CORE-5A PRISMA-PI source A/B + processed outputs armed");
    prisma_legacy_main();
}
