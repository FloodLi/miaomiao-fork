#include "http_receive.h"

#include "esp_timer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

void http_receive_init(http_receive_t *state, uint32_t total_ms)
{
    state->started_us = esp_timer_get_time();
    state->progress_us = state->started_us;
    state->total_us = (int64_t)total_ms * 1000;
    state->failed = false;
}

void http_receive_abort(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;
    /* lwIP uses SO_SNDTIMEO to bound FIN allocation retries during shutdown.
     * Do not close() here: httpd still owns this fd. Shutting down both directions
     * also prevents httpd's automatic body purge from waiting on a stalled peer. */
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)shutdown(fd, SHUT_RDWR);
}

static int64_t remaining_us(const http_receive_t *state, int64_t now)
{
    int64_t idle = (int64_t)HTTP_RECEIVE_IDLE_MS * 1000 - (now - state->progress_us);
    int64_t total = state->total_us - (now - state->started_us);
    return idle < total ? idle : total;
}

int http_receive_chunk(httpd_req_t *req, http_receive_t *state,
                       char *buf, size_t size)
{
    if (state->failed) return HTTPD_SOCK_ERR_FAIL;
    int fd = httpd_req_to_sockfd(req);
    struct timeval saved;
    socklen_t saved_len = sizeof(saved);
    bool have_saved = fd >= 0 &&
        getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &saved, &saved_len) == 0;
    if (!have_saved || !buf || size == 0) goto fail;

    for (;;) {
        int64_t left = remaining_us(state, esp_timer_get_time());
        if (left <= 0) goto fail;
        /* A one-second poll bounds each call even with pending header/body data.
         * Always restore the normal timeout for the next keep-alive request. */
        if (left > 1000000) left = 1000000;
        /* lwIP truncates timeval to milliseconds; zero means wait forever. */
        left = ((left + 999) / 1000) * 1000;
        struct timeval timeout = {
            .tv_sec = (long)(left / 1000000),
            .tv_usec = (long)(left % 1000000),
        };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
            goto fail;
        int received = httpd_req_recv(req, buf, size);
        int64_t now = esp_timer_get_time();
        /* Check before recording progress: late data must not revive a deadline. */
        if (remaining_us(state, now) <= 0) goto fail;
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) goto fail;
        state->progress_us = now;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &saved, sizeof(saved)) != 0)
            goto fail;
        return received;
    }

fail:
    state->failed = true;
    /* Leave a short receive timeout on failure as a fallback for socket errors. */
    http_receive_abort(req);
    return HTTPD_SOCK_ERR_FAIL;
}

bool http_receive_drain(httpd_req_t *req)
{
    if (!req) return false;
    http_receive_t state;
    http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
    size_t remaining = req->content_len;
    char buf[128];
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int received = http_receive_chunk(req, &state, buf, wanted);
        if (received <= 0) return false;
        remaining -= (size_t)received;
    }
    return true;
}

bool http_receive_commit_file(const char *temporary, const char *destination,
                              const char *backup)
{
    struct stat st;
    if (stat(backup, &st) == 0 || errno != ENOENT) return false;
    bool had_old = stat(destination, &st) == 0;
    if (!had_old && errno != ENOENT) return false;
    if (had_old && rename(destination, backup) != 0) return false;
    if (rename(temporary, destination) != 0) {
        /* If the filesystem also rejects rollback, retain backup for recovery. */
        if (had_old) (void)rename(backup, destination);
        return false;
    }
    if (had_old) (void)remove(backup);
    return true;
}
