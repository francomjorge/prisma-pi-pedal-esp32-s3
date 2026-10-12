#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "storage.h"

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

bool prisma_appliance_get_on(void){return prisma_shared_on;}
int prisma_appliance_get_mode(void){return prisma_shared_mode ? 1 : 0;}
int prisma_appliance_get_intensity(void){return prisma_shared_intensity;}
const char *prisma_appliance_get_filename(void){return prisma_storage_current();}
bool prisma_appliance_get_effect(int effect){if(effect<0||effect>=EFFECT_COUNT)return false;if(effect==5)return true;return effect_enabled[effect];}
int prisma_appliance_get_effect_value(int effect){if(effect<0||effect>=EFFECT_COUNT)return 0;return effect_value[effect];}

void app_main(void)
{
    prisma_shared_mode = 0;
    prisma_shared_intensity = 61;
    prisma_storage_init();
    ESP_LOGI(SYNC_TAG, "M4.9 GLOBAL WEB SYNC ACTIVE - DIRECT APPLIANCE GETTERS");
    prisma_legacy_main();
}
