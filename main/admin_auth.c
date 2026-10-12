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

/* Bootstrap password is NOT stored in source. Only this salted SHA-256 digest is compiled in.
   The plaintext bootstrap password is distributed out-of-band and can later be replaced in NVS. */
static const uint8_t BOOTSTRAP_HASH[ADMIN_HASH_LEN] = {
    0x01, 0xcf, 0x9a, 0xaf, 0x56, 0x59, 0x70, 0xe0,
    0x3e, 0x50, 0x5a, 0x30, 0x6e, 0x72, 0x4d, 0xac,
    0xae, 0x24, 0xef, 0x5e, 0x6c, 0x89, 0xa0, 0x41,
    0xd5, 0xab, 0x7f, 0x5a, 0xd3, 0xa8, 0xdc, 0x3e
};

static void challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"PRISMA-P ADMIN\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "PRISMA-P admin authentication required");
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
        memcpy(out, BOOTSTRAP_HASH, ADMIN_HASH_LEN);
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
    if (len < 12 || len > 64) return ESP_ERR_INVALID_ARG;

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
    if (len < 7 || len >= ADMIN_AUTH_MAX) {
        challenge(req);
        return false;
    }

    char auth[ADMIN_AUTH_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strncmp(auth, "Basic ", 6) != 0) {
        challenge(req);
        return false;
    }

    unsigned char decoded[ADMIN_DECODED_MAX];
    size_t decoded_len = 0;
    const unsigned char *encoded = (const unsigned char *)(auth + 6);
    size_t encoded_len = strlen(auth + 6);
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, encoded, encoded_len);
    memset(auth, 0, sizeof(auth));
    if (rc != 0 || decoded_len == 0 || decoded_len >= sizeof(decoded)) {
        challenge(req);
        return false;
    }
    decoded[decoded_len] = '\0';

    char *separator = strchr((char *)decoded, ':');
    if (!separator) {
        memset(decoded, 0, sizeof(decoded));
        challenge(req);
        return false;
    }
    *separator = '\0';
    const char *username = (const char *)decoded;
    const char *password = separator + 1;

    uint8_t expected[ADMIN_HASH_LEN];
    uint8_t actual[ADMIN_HASH_LEN];
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
