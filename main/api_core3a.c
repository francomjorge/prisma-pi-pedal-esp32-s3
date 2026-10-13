#include <stdio.h>
#include <string.h>

#include "admin_api.h"
#include "admin_auth.h"
#include "storage_verify_api.h"
#include "process_api.h"
#include "cJSON.h"
#include "diagnostics.h"
#include "image_ab.h"
#include "video_ab.h"
#include "wifi_portal.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

static const char *TAG = "PRISMA_API3";

esp_err_t __real_httpd_start(httpd_handle_t *handle, const httpd_config_t *config);

static void no_cache(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "Expires", "0");
}

static esp_err_t send_json(httpd_req_t *r, cJSON *root)
{
    if (!root) return ESP_ERR_NO_MEM;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(r, "application/json");
    no_cache(r);
    esp_err_t err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static void make_identity(char *device_id, size_t device_id_size,
                          char *hostname, size_t hostname_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strlcpy(device_id, "PRP-UNKNOWN", device_id_size);
        strlcpy(hostname, "prisma-pi-unknown", hostname_size);
        return;
    }
    snprintf(device_id, device_id_size, "PRP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(hostname, hostname_size, "prisma-pi-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static cJSON *network_json(void)
{
    cJSON *net = cJSON_CreateObject();
    if (!net) return NULL;

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    wifi_ap_record_t ap = {0};
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

    const char *mode_name = "off";
    if (connected) mode_name = "sta";
    else if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) mode_name = "provisioning";
    else if (mode == WIFI_MODE_STA) mode_name = "sta_disconnected";

    cJSON_AddStringToObject(net, "mode", mode_name);
    cJSON_AddBoolToObject(net, "connected", connected);
    cJSON_AddStringToObject(net, "ssid", connected ? (const char *)ap.ssid : "");
    if (connected) cJSON_AddNumberToObject(net, "rssi", ap.rssi);
    else cJSON_AddNullToObject(net, "rssi");

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    char ip_text[16] = "";
    char gateway[16] = "";
    char netmask[16] = "";
    char dns_text[16] = "";

    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip.ip));
        snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip.gw));
        snprintf(netmask, sizeof(netmask), IPSTR, IP2STR(&ip.netmask));
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            snprintf(dns_text, sizeof(dns_text), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    }

    cJSON_AddStringToObject(net, "ip", ip_text);
    cJSON_AddStringToObject(net, "gateway", gateway);
    cJSON_AddStringToObject(net, "netmask", netmask);
    cJSON_AddStringToObject(net, "dns", dns_text);

    wifi_config_t ap_cfg = {0};
    if ((mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
        esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
        cJSON_AddStringToObject(net, "ap_ssid", (const char *)ap_cfg.ap.ssid);
    } else {
        cJSON_AddStringToObject(net, "ap_ssid", "");
    }
    return net;
}

static cJSON *media_json(void)
{
    cJSON *media = cJSON_CreateObject();
    if (!media) return NULL;
    prisma_image_info_t image = {0};
    prisma_video_info_t video = {0};
    bool image_ok = prisma_image_ab_get_info(&image) == ESP_OK;
    bool video_ok = prisma_video_ab_get_info(&video) == ESP_OK;

    cJSON *ij = cJSON_AddObjectToObject(media, "image");
    if (ij) {
        cJSON_AddBoolToObject(ij, "ready", image_ok && image.ready);
        cJSON_AddBoolToObject(ij, "valid", image_ok && image.valid);
        char bank[2] = { image_ok ? image.active_bank : '-', 0 };
        cJSON_AddStringToObject(ij, "active_bank", bank);
        cJSON_AddNumberToObject(ij, "generation", image_ok ? image.generation : 0);
        cJSON_AddNumberToObject(ij, "size", image_ok ? image.size : 0);
    }

    cJSON *vj = cJSON_AddObjectToObject(media, "video");
    if (vj) {
        cJSON_AddBoolToObject(vj, "ready", video_ok && video.ready);
        cJSON_AddBoolToObject(vj, "valid", video_ok && video.valid);
        char bank[2] = { video_ok ? video.active_bank : '-', 0 };
        cJSON_AddStringToObject(vj, "active_bank", bank);
        cJSON_AddNumberToObject(vj, "generation", video_ok ? video.generation : 0);
        cJSON_AddNumberToObject(vj, "size", video_ok ? video.size : 0);
    }
    return media;
}

static esp_err_t health_get(httpd_req_t *r)
{
    char device_id[20];
    char hostname[32];
    make_identity(device_id, sizeof(device_id), hostname, sizeof(hostname));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)prisma_diagnostics_uptime_ms());
    return send_json(r, root);
}

static esp_err_t control_status_get(httpd_req_t *r)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scope", "control");
    cJSON_AddBoolToObject(root, "on", prisma_appliance_get_on());
    cJSON_AddNumberToObject(root, "mode", prisma_appliance_get_mode());
    cJSON_AddNumberToObject(root, "intensity", prisma_appliance_get_intensity());
    cJSON_AddStringToObject(root, "source", prisma_appliance_get_filename());
    cJSON *effects = cJSON_AddArrayToObject(root, "effects");
    cJSON *values = cJSON_AddArrayToObject(root, "values");
    for (int i = 0; i < 6; i++) {
        cJSON_AddItemToArray(effects, cJSON_CreateBool(prisma_appliance_get_effect(i)));
        cJSON_AddItemToArray(values, cJSON_CreateNumber(prisma_appliance_get_effect_value(i)));
    }
    return send_json(r, root);
}

static esp_err_t admin_status_get(httpd_req_t *r)
{
    if (!prisma_admin_authorize(r)) return ESP_OK;
    char device_id[20];
    char hostname[32];
    make_identity(device_id, sizeof(device_id), hostname, sizeof(hostname));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scope", "admin");
    cJSON_AddStringToObject(root, "api", "v1");
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "hostname", hostname);

    cJSON *security = cJSON_AddObjectToObject(root, "security");
    if (security) {
        cJSON_AddBoolToObject(security, "admin_auth", true);
        cJSON_AddStringToObject(security, "scheme", "http-basic");
        cJSON_AddStringToObject(security, "username", PRISMA_ADMIN_USERNAME);
        cJSON_AddStringToObject(security, "transport", "http-lan");
    }
    cJSON *system = prisma_diagnostics_system_json();
    if (system) cJSON_AddItemToObject(root, "system", system);
    cJSON *network = network_json();
    if (network) cJSON_AddItemToObject(root, "network", network);
    cJSON *media = media_json();
    if (media) cJSON_AddItemToObject(root, "media", media);

    cJSON *discovery = cJSON_AddObjectToObject(root, "discovery");
    if (discovery) {
        char local_name[40];
        snprintf(local_name, sizeof(local_name), "%s.local", hostname);
        cJSON_AddStringToObject(discovery, "mdns", local_name);
        cJSON_AddStringToObject(discovery, "service", "_prisma._tcp");
        cJSON_AddNumberToObject(discovery, "http_port", 80);
        cJSON_AddNumberToObject(discovery, "udp_port", 4210);
        cJSON_AddStringToObject(discovery, "udp_magic", "PRISMA_DISCOVER_V1");
    }
    return send_json(r, root);
}

static void register_core3_routes(httpd_handle_t server)
{
    static const httpd_uri_t health = {
        .uri = "/api/v1/health", .method = HTTP_GET, .handler = health_get
    };
    static const httpd_uri_t control_status = {
        .uri = "/api/v1/control/status", .method = HTTP_GET, .handler = control_status_get
    };
    static const httpd_uri_t admin_status = {
        .uri = "/api/v1/admin/status", .method = HTTP_GET, .handler = admin_status_get
    };
    esp_err_t a = httpd_register_uri_handler(server, &health);
    esp_err_t b = httpd_register_uri_handler(server, &control_status);
    esp_err_t c = httpd_register_uri_handler(server, &admin_status);
    if (a != ESP_OK || b != ESP_OK || c != ESP_OK) {
        ESP_LOGE(TAG, "CORE-3 route registration failed health=%s control=%s admin=%s",
                 esp_err_to_name(a), esp_err_to_name(b), esp_err_to_name(c));
    } else {
        ESP_LOGI(TAG, "CORE-5A API ready: diagnostics + source A/B + processed outputs");
    }
}

esp_err_t __wrap_httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    httpd_config_t expanded = *config;
    if (expanded.max_uri_handlers < 48) expanded.max_uri_handlers = 48;
    esp_err_t err = __real_httpd_start(handle, &expanded);
    if (err != ESP_OK) return err;
    esp_err_t diag = prisma_diagnostics_init();
    if (diag != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics persistence unavailable: %s", esp_err_to_name(diag));
    }
    prisma_admin_register_routes(*handle);
    prisma_storage_verify_register_routes(*handle);
    prisma_process_register_routes(*handle);
    register_core3_routes(*handle);
    return ESP_OK;
}
