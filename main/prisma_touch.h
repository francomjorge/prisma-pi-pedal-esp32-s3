#pragma once
#include <stdbool.h>
#include <stdint.h>

bool prisma_touch_init(void);
bool prisma_touch_read(uint16_t *x, uint16_t *y);
