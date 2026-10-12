#pragma once
#include <stdbool.h>
#include <stdint.h>

bool prisma_display_init(void);
void prisma_display_boot_stage(unsigned stage, bool ok);
void prisma_display_touch(uint16_t x, uint16_t y);
void prisma_display_swipe(bool left);
