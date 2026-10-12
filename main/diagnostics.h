#pragma once

#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t prisma_diagnostics_init(void);
uint32_t prisma_diagnostics_boot_count(void);
uint64_t prisma_diagnostics_uptime_ms(void);
const char *prisma_diagnostics_reset_reason(void);
cJSON *prisma_diagnostics_system_json(void);
