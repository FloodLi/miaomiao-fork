#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#define HTTP_RECEIVE_IDLE_MS 30000
#define HTTP_RECEIVE_CONFIG_MS 60000
#define HTTP_RECEIVE_UPLOAD_MS 300000

typedef struct {
    int64_t started_us;
    int64_t progress_us;
    int64_t total_us;
    bool failed;
} http_receive_t;

/* One state per body, shared across all chunks and any subsequent drain. */
void http_receive_init(http_receive_t *state, uint32_t total_ms);
/* Retries transient timeouts internally; <= 0 is terminal and shuts down RX/TX.
 * The server retains ownership of the descriptor and performs the final close. */
int http_receive_chunk(httpd_req_t *req, http_receive_t *state,
                       char *buf, size_t size);
/* Use on local failures before the full body is consumed, e.g. a disk error. */
void http_receive_abort(httpd_req_t *req);
/* Drain a complete, previously unread body. Never execute actions after failure. */
bool http_receive_drain(httpd_req_t *req);
/* SPIFFS cannot rename over an existing file. Stage the old file as backup and
 * restore it if promotion fails. Paths must be distinct, on the same filesystem.
 * A pre-existing backup is preserved and causes failure for manual recovery. */
bool http_receive_commit_file(const char *temporary, const char *destination,
                              const char *backup);
