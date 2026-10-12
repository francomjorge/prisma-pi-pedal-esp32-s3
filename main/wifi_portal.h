#pragma once
#include <stdbool.h>

void prisma_wifi_start(void);
bool prisma_appliance_get_on(void);
int prisma_appliance_get_mode(void);
int prisma_appliance_get_intensity(void);
const char *prisma_appliance_get_filename(void);
bool prisma_appliance_get_effect(int effect);
int prisma_appliance_get_effect_value(int effect);
bool prisma_control_get_on(void);
int prisma_control_get_mode(void);
int prisma_control_get_intensity(void);
int prisma_control_get_generation(void);
void prisma_control_set_on(bool on);
void prisma_control_set_mode(int mode);
void prisma_control_set_intensity(int intensity);
void prisma_control_set_generation(int generation);
void prisma_control_set_filename(const char *filename);
void prisma_control_set_effect(int effect, bool enabled);
bool prisma_control_get_effect(int effect);
void prisma_control_set_effect_value(int effect, int value);
int prisma_control_get_effect_value(int effect);
