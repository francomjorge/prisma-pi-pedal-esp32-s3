#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#define PRISMA_ADMIN_USERNAME "Prisma-P"
#define PRISMA_ADMIN_PASSWORD "Prisma-P"

bool prisma_admin_authorize(httpd_req_t *req);
