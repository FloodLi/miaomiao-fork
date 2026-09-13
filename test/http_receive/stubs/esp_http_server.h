#pragma once
#include <stddef.h>
#define HTTPD_SOCK_ERR_FAIL (-1)
#define HTTPD_SOCK_ERR_INVALID (-2)
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
typedef struct { size_t content_len; int fd; } httpd_req_t;
int httpd_req_to_sockfd(httpd_req_t *req);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t size);
