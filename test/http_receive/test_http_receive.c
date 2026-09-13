#define _POSIX_C_SOURCE 200809L
#include "http_receive.h"
#include "lwip/sockets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t now_us, receive_timeout, send_timeout, event_due;
static int64_t event_repeat_us;
static int recv_calls, close_calls, event_result, set_calls, fail_set_call;
static bool socket_closed, fail_get;
static httpd_req_t req;

static void reset(void)
{
    now_us = 1000000000000LL;
    receive_timeout = 8000000;
    send_timeout = 8000000;
    event_due = INT64_MAX;
    event_repeat_us = 0;
    recv_calls = close_calls = set_calls = fail_set_call = 0;
    event_result = 1;
    socket_closed = fail_get = false;
    req = (httpd_req_t){ .content_len = 4, .fd = 7 };
}

int64_t esp_timer_get_time(void) { return now_us; }
int httpd_req_to_sockfd(httpd_req_t *request) { return request ? request->fd : -1; }

int fake_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    assert(fd == 7 && level == SOL_SOCKET && option == SO_RCVTIMEO);
    assert(*length == sizeof(struct timeval));
    if (fail_get) return -1;
    *(struct timeval *)value = (struct timeval){ receive_timeout / 1000000,
                                               receive_timeout % 1000000 };
    return 0;
}

int fake_setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    assert(fd == 7 && level == SOL_SOCKET && length == sizeof(struct timeval));
    if (++set_calls == fail_set_call) return -1;
    const struct timeval *timeout = value;
    int64_t us = (int64_t)timeout->tv_sec * 1000000 + timeout->tv_usec;
    assert(us > 0);
    if (option == SO_RCVTIMEO) receive_timeout = us;
    else { assert(option == SO_SNDTIMEO); send_timeout = us; }
    return 0;
}

int fake_shutdown(int fd, int how)
{
    assert(fd == 7 && how == SHUT_RDWR);
    assert(send_timeout == 1000000);
    socket_closed = true;
    close_calls++;
    return 0;
}

int httpd_req_recv(httpd_req_t *request, char *buf, size_t size)
{
    assert(request == &req && size > 0);
    recv_calls++;
    if (socket_closed) return 0;
    assert(receive_timeout > 0 && receive_timeout <= 1000000);
    if (event_due > now_us + receive_timeout) {
        now_us += receive_timeout;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    now_us = event_due;
    event_due = event_repeat_us ? now_us + event_repeat_us : INT64_MAX;
    if (event_result > 0) {
        assert((size_t)event_result <= size);
        memset(buf, 'x', (size_t)event_result);
    }
    return event_result;
}

static void no_progress_and_cleanup(void)
{
    reset();
    http_receive_t state;
    http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
    int64_t start = now_us;
    char buf[4];
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(now_us - start == 30000000 && recv_calls == 30 && close_calls == 1);
    int calls = recv_calls;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(recv_calls == calls && close_calls == 1);
    /* Model httpd_req_delete's purge after a handler returns ESP_OK. */
    assert(httpd_req_recv(&req, buf, sizeof(buf)) == 0);
    assert(now_us - start == 30000000);
}

static void trickle_total(uint32_t total_ms)
{
    reset();
    http_receive_t state;
    http_receive_init(&state, total_ms);
    int64_t start = now_us;
    char buf[4];
    for (uint32_t elapsed = 20000; elapsed < total_ms; elapsed += 20000) {
        event_due = now_us + 20000000;
        assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) == 1);
        assert(receive_timeout == 8000000 && !socket_closed);
    }
    event_due = start + (int64_t)total_ms * 1000;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(now_us - start == (int64_t)total_ms * 1000 && socket_closed);
}

static void progress_and_boundaries(void)
{
    reset();
    http_receive_t state;
    http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
    char buf[4];
    event_due = now_us + 29000000;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) == 1);
    event_due = now_us + 29000000;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) == 1);
    assert(receive_timeout == 8000000);

    reset();
    http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
    event_due = now_us + 30000000;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);

    reset();
    http_receive_init(&state, 1500);
    int64_t start = now_us;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(now_us - start == 1500000 && recv_calls == 2);

    reset();
    http_receive_init(&state, 1);
    now_us += 999;
    start = now_us;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(now_us - start == 1000 && recv_calls == 1);

    reset();
    http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
    now_us += 60000000;
    assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
    assert(recv_calls == 0);
}

static void terminal_errors(void)
{
    for (int result = -2; result <= 0; result++) {
        reset();
        http_receive_t state;
        http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
        event_due = now_us;
        event_result = result;
        char buf[4];
        assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
        assert(socket_closed && recv_calls == 1);
    }
    for (int failure = 0; failure < 3; failure++) {
        reset();
        http_receive_t state;
        http_receive_init(&state, HTTP_RECEIVE_CONFIG_MS);
        fail_get = failure == 0;
        fail_set_call = failure;
        event_due = now_us;
        char buf[4];
        assert(http_receive_chunk(&req, &state, buf, sizeof(buf)) < 0);
        assert(socket_closed);
    }
}

static void drain_body(void)
{
    reset();
    req.content_len = 0;
    assert(http_receive_drain(&req) && recv_calls == 0 && !socket_closed);
    req.content_len = 4;
    event_due = now_us + 2000000;
    event_result = 4;
    assert(http_receive_drain(&req) && receive_timeout == 8000000);
    reset();
    event_due = now_us;
    event_result = 2;
    assert(!http_receive_drain(&req) && socket_closed);
}

static void write_file(const char *path, const char *data)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, strlen(data), file) == strlen(data));
    assert(fclose(file) == 0);
}

static void expect_file(const char *path, const char *data)
{
    FILE *file = fopen(path, "rb");
    char buf[32] = {0};
    assert(file && fread(buf, 1, sizeof(buf) - 1, file) == strlen(data));
    assert(strcmp(buf, data) == 0);
    assert(fclose(file) == 0);
}

static void staged_file_commit(void)
{
    char directory[] = "/tmp/msp-http-receive-XXXXXX";
    char original[4096];
    assert(getcwd(original, sizeof(original)));
    assert(mkdtemp(directory) && chdir(directory) == 0);
    write_file("original", "valid original");
    write_file("incoming", "incomplete");
    no_progress_and_cleanup();
    assert(remove("incoming") == 0);
    expect_file("original", "valid original");
    write_file("incoming", "complete upload");
    assert(http_receive_commit_file("incoming", "original", "backup"));
    expect_file("original", "complete upload");
    assert(access("backup", F_OK) != 0 && access("incoming", F_OK) != 0);
    /* Promotion failure must roll the existing destination back. */
    assert(!http_receive_commit_file("missing", "original", "backup"));
    expect_file("original", "complete upload");
    assert(access("backup", F_OK) != 0);
    write_file("backup", "recovery copy");
    write_file("incoming", "next upload");
    assert(!http_receive_commit_file("incoming", "original", "backup"));
    expect_file("backup", "recovery copy");
    expect_file("original", "complete upload");
    assert(remove("backup") == 0 && remove("original") == 0);
    assert(http_receive_commit_file("incoming", "original", "backup"));
    expect_file("original", "next upload");
    assert(remove("original") == 0);
    assert(chdir(original) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    no_progress_and_cleanup();
    trickle_total(HTTP_RECEIVE_CONFIG_MS);
    trickle_total(HTTP_RECEIVE_UPLOAD_MS);
    progress_and_boundaries();
    terminal_errors();
    drain_body();
    staged_file_commit();
    puts("http_receive: all tests passed");
    return 0;
}
