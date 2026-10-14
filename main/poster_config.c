#include "poster_config.h"

#include <limits.h>
#include <string.h>

#include "nvs.h"

#define NS "poster_cfg"

static bool preset_valid(prisma_poster_preset_t p)
{
    return p == PRISMA_POSTER_PRESET_AUTO || p == PRISMA_POSTER_PRESET_720 ||
           p == PRISMA_POSTER_PRESET_480 || p == PRISMA_POSTER_PRESET_360 ||
           p == PRISMA_POSTER_PRESET_240 || p == PRISMA_POSTER_PRESET_166;
}

static bool aspect_valid(prisma_poster_aspect_t a)
{
    return a >= PRISMA_POSTER_ASPECT_SOURCE && a <= PRISMA_POSTER_ASPECT_1_1;
}

void prisma_poster_config_defaults(prisma_poster_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->image_preset = PRISMA_POSTER_PRESET_AUTO;
    out->image_aspect = PRISMA_POSTER_ASPECT_SOURCE;
    out->video_preset = PRISMA_POSTER_PRESET_360;
    out->video_aspect = PRISMA_POSTER_ASPECT_SOURCE;
}

bool prisma_poster_config_valid(const prisma_poster_config_t *cfg)
{
    return cfg && preset_valid(cfg->image_preset) && preset_valid(cfg->video_preset) &&
           aspect_valid(cfg->image_aspect) && aspect_valid(cfg->video_aspect);
}

esp_err_t prisma_poster_config_load(prisma_poster_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    prisma_poster_config_defaults(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint16_t ip = (uint16_t)out->image_preset;
    uint8_t ia = (uint8_t)out->image_aspect;
    uint16_t vp = (uint16_t)out->video_preset;
    uint8_t va = (uint8_t)out->video_aspect;
    (void)nvs_get_u16(h, "ipreset", &ip);
    (void)nvs_get_u8(h, "iaspect", &ia);
    (void)nvs_get_u16(h, "vpreset", &vp);
    (void)nvs_get_u8(h, "vaspect", &va);
    nvs_close(h);

    out->image_preset = (prisma_poster_preset_t)ip;
    out->image_aspect = (prisma_poster_aspect_t)ia;
    out->video_preset = (prisma_poster_preset_t)vp;
    out->video_aspect = (prisma_poster_aspect_t)va;
    if (!prisma_poster_config_valid(out)) prisma_poster_config_defaults(out);
    return ESP_OK;
}

esp_err_t prisma_poster_config_save(const prisma_poster_config_t *cfg)
{
    if (!prisma_poster_config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, "ipreset", (uint16_t)cfg->image_preset);
    if (err == ESP_OK) err = nvs_set_u8(h, "iaspect", (uint8_t)cfg->image_aspect);
    if (err == ESP_OK) err = nvs_set_u16(h, "vpreset", (uint16_t)cfg->video_preset);
    if (err == ESP_OK) err = nvs_set_u8(h, "vaspect", (uint8_t)cfg->video_aspect);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static uint16_t scaled(uint32_t n, uint32_t d)
{
    if (!d) return 1;
    uint32_t v = (n + d / 2u) / d;
    if (v < 1u) v = 1u;
    if (v > UINT16_MAX) v = UINT16_MAX;
    return (uint16_t)v;
}

bool prisma_poster_resolve_output(prisma_poster_preset_t preset,
                                  prisma_poster_aspect_t aspect,
                                  uint16_t source_width,
                                  uint16_t source_height,
                                  uint16_t *out_width,
                                  uint16_t *out_height)
{
    if (!out_width || !out_height || !source_width || !source_height ||
        !preset_valid(preset) || !aspect_valid(aspect)) return false;

    uint16_t source_major = source_width > source_height ? source_width : source_height;
    uint16_t major = preset == PRISMA_POSTER_PRESET_AUTO
                   ? source_major
                   : (uint16_t)preset;

    if (aspect == PRISMA_POSTER_ASPECT_SOURCE) {
        if (preset == PRISMA_POSTER_PRESET_AUTO) {
            *out_width = source_width;
            *out_height = source_height;
            return true;
        }
        if (source_width >= source_height) {
            *out_width = major;
            *out_height = scaled((uint32_t)source_height * major, source_width);
        } else {
            *out_height = major;
            *out_width = scaled((uint32_t)source_width * major, source_height);
        }
        return true;
    }

    switch (aspect) {
        case PRISMA_POSTER_ASPECT_16_9:
            *out_width = major; *out_height = scaled((uint32_t)major * 9u, 16u); break;
        case PRISMA_POSTER_ASPECT_9_16:
            *out_height = major; *out_width = scaled((uint32_t)major * 9u, 16u); break;
        case PRISMA_POSTER_ASPECT_4_3:
            *out_width = major; *out_height = scaled((uint32_t)major * 3u, 4u); break;
        case PRISMA_POSTER_ASPECT_3_4:
            *out_height = major; *out_width = scaled((uint32_t)major * 3u, 4u); break;
        case PRISMA_POSTER_ASPECT_1_1:
            *out_width = major; *out_height = major; break;
        default:
            return false;
    }
    return true;
}

uint32_t prisma_poster_config_hash(const prisma_poster_config_t *cfg, bool video)
{
    if (!cfg) return 0;
    uint32_t h = 2166136261u;
    uint32_t values[4] = {
        0x504F5354u,
        2u,
        video ? (uint32_t)cfg->video_preset : (uint32_t)cfg->image_preset,
        video ? (uint32_t)cfg->video_aspect : (uint32_t)cfg->image_aspect,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint32_t v = values[i];
        for (int b = 0; b < 4; ++b) {
            h ^= (uint8_t)(v & 0xFFu);
            h *= 16777619u;
            v >>= 8;
        }
    }
    return h;
}
