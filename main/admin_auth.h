#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define PRISMA_ADMIN_USERNAME "Prisma-P"

bool prisma_admin_authorize(httpd_req_t *req);
esp_err_t prisma_admin_set_password(const char *password);
