#include <string.h>

#include "admin_auth.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define ADMIN_NS "prisma_admin"
#define ADMIN_HASH_KEY "pwd_hash"
#define ADMIN_HASH_LEN 32
#define ADMIN_AUTH_MAX 128
#define ADMIN_DECODED_MAX 96

static const char HASH_PREFIX[] = "prisma-admin-v1:";

/* Fixed onboarding password is never stored in plaintext in the repository.
   FACTORY_HASH = SHA-256(HASH_PREFIX + factory password). */
static const uint8_t FACTORY_HASH[ADMIN_HASH_LEN] = {
    0x05,0x94,0x9f,0x66,0x45,0x40,0x10,0x50,
    0x26,0x41,0x16,0x8e,0x3b,0xe6,0xed,0x19,
    0xeb,0x29,0xaa,0xdf,0x83,0x4e,0x4a,0x68,
    0xc9,0xae,0x05,0xbf,0x9b,0x8c,0xea,0x2c
};

/* Previous validated factory default, used only to migrate an unchanged unit.
   User-selected passwords do not match this digest and are preserved. */
static const uint8_t LEGACY_FACTORY_HASH[ADMIN_HASH_LEN] = {
    0xc6,0xb1,0x59,0xbf,0x94,0xe9,0x38,0x2d,
    0xa7,0x7b,0x05,0x32,0xa7,0x36,0x4f,0xb8,
    0x85,0xac,0x94,0xd5,0xe3,0xb8,0xf0,0x37,
    0x63,0xac,0x2b,0x6f,0x42,0x07,0x18,0xe3
};

static void challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"PRISMA-PI ADMIN\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "PRISMA-PI admin authentication required");
}

static void hash_password(const char *password, uint8_t out[ADMIN_HASH_LEN])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    (void)mbedtls_sha256_starts(&ctx, 0);
    (void)mbedtls_sha256_update(&ctx, (const unsigned char *)HASH_PREFIX, strlen(HASH_PREFIX));
    (void)mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    (void)mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static esp_err_t load_or_init_hash(uint8_t out[ADMIN_HASH_LEN])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ADMIN_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t len = ADMIN_HASH_LEN;
    err = nvs_get_blob(h, ADMIN_HASH_KEY, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len != ADMIN_HASH_LEN) {
        memcpy(out, FACTORY_HASH, ADMIN_HASH_LEN);
        err = nvs_set_blob(h, ADMIN_HASH_KEY, out, ADMIN_HASH_LEN);
        if (err == ESP_OK) err = nvs_commit(h);
    } else if (err == ESP_OK && constant_time_equal(out, LEGACY_FACTORY_HASH, ADMIN_HASH_LEN)) {
        memcpy(out, FACTORY_HASH, ADMIN_HASH_LEN);
        err = nvs_set_blob(h, ADMIN_HASH_KEY, out, ADMIN_HASH_LEN);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t prisma_admin_set_password(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(password);
    if (len < 8 || len > 64) return ESP_ERR_INVALID_ARG;
    uint8_t digest[ADMIN_HASH_LEN];
    hash_password(password, digest);
    nvs_handle_t h;
    esp_err_t err = nvs_open(ADMIN_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, ADMIN_HASH_KEY, digest, sizeof(digest));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    memset(digest, 0, sizeof(digest));
    return err;
}

bool prisma_admin_authorize(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len < 7 || len >= ADMIN_AUTH_MAX) { challenge(req); return false; }

    char auth[ADMIN_AUTH_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strncmp(auth, "Basic ", 6) != 0) { challenge(req); return false; }

    unsigned char decoded[ADMIN_DECODED_MAX];
    size_t decoded_len = 0;
    const unsigned char *encoded = (const unsigned char *)(auth + 6);
    size_t encoded_len = strlen(auth + 6);
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, encoded, encoded_len);
    memset(auth, 0, sizeof(auth));
    if (rc != 0 || decoded_len == 0 || decoded_len >= sizeof(decoded)) { challenge(req); return false; }
    decoded[decoded_len] = '\0';

    char *separator = strchr((char *)decoded, ':');
    if (!separator) { memset(decoded, 0, sizeof(decoded)); challenge(req); return false; }
    *separator = '\0';
    const char *username = (const char *)decoded;
    const char *password = separator + 1;

    uint8_t expected[ADMIN_HASH_LEN] = {0};
    uint8_t actual[ADMIN_HASH_LEN] = {0};
    bool ok = false;
    if (strcmp(username, PRISMA_ADMIN_USERNAME) == 0 && load_or_init_hash(expected) == ESP_OK) {
        hash_password(password, actual);
        ok = constant_time_equal(expected, actual, ADMIN_HASH_LEN);
    }
    memset(expected, 0, sizeof(expected));
    memset(actual, 0, sizeof(actual));
    memset(decoded, 0, sizeof(decoded));
    if (!ok) challenge(req);
    return ok;
}
