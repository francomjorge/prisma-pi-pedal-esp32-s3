#include <string.h>
#include "admin_auth.h"

#define PRISMA_ADMIN_BASIC "Basic UHJpc21hLVA6UHJpc21hLVA="

static void challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"PRISMA-P ADMIN\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "PRISMA-P admin authentication required");
}

bool prisma_admin_authorize(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 95) {
        challenge(req);
        return false;
    }

    char auth[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strcmp(auth, PRISMA_ADMIN_BASIC) != 0) {
        challenge(req);
        return false;
    }
    return true;
}
