#include "poster_config.h"

#include <string.h>

#include "nvs.h"

#define NS "poster_cfg"
#define DEFAULT_W 360
#define DEFAULT_H 360

void prisma_poster_config_defaults(prisma_poster_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->image_mode = PRISMA_POSTER_IMAGE_SOURCE;
    out->image_width = DEFAULT_W;
    out->image_height = DEFAULT_H;
    out->video_width = DEFAULT_W;
    out->video_height = DEFAULT_H;
}

bool prisma_poster_config_valid(const prisma_poster_config_t *cfg)
{
    if (!cfg) return false;
    if (cfg->image_mode != PRISMA_POSTER_IMAGE_SOURCE &&
        cfg->image_mode != PRISMA_POSTER_IMAGE_MANUAL) return false;
    if (cfg->image_width < 64 || cfg->image_width > 1024) return false;
    if (cfg->image_height < 64 || cfg->image_height > 1024) return false;
    if (cfg->video_width < 64 || cfg->video_width > 640) return false;
    if (cfg->video_height < 64 || cfg->video_height > 640) return false;
    return true;
}

esp_err_t prisma_poster_config_load(prisma_poster_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    prisma_poster_config_defaults(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t mode = (uint8_t)out->image_mode;
    uint16_t iw = out->image_width, ih = out->image_height;
    uint16_t vw = out->video_width, vh = out->video_height;
    (void)nvs_get_u8(h, "imode", &mode);
    (void)nvs_get_u16(h, "iw", &iw);
    (void)nvs_get_u16(h, "ih", &ih);
    (void)nvs_get_u16(h, "vw", &vw);
    (void)nvs_get_u16(h, "vh", &vh);
    nvs_close(h);

    out->image_mode = (prisma_poster_image_mode_t)mode;
    out->image_width = iw;
    out->image_height = ih;
    out->video_width = vw;
    out->video_height = vh;
    if (!prisma_poster_config_valid(out)) prisma_poster_config_defaults(out);
    return ESP_OK;
}

esp_err_t prisma_poster_config_save(const prisma_poster_config_t *cfg)
{
    if (!prisma_poster_config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "imode", (uint8_t)cfg->image_mode);
    if (err == ESP_OK) err = nvs_set_u16(h, "iw", cfg->image_width);
    if (err == ESP_OK) err = nvs_set_u16(h, "ih", cfg->image_height);
    if (err == ESP_OK) err = nvs_set_u16(h, "vw", cfg->video_width);
    if (err == ESP_OK) err = nvs_set_u16(h, "vh", cfg->video_height);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
