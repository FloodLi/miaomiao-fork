#pragma once
#include <sys/socket.h>
#include <sys/time.h>
int fake_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
int fake_setsockopt(int fd, int level, int option, const void *value, socklen_t length);
int fake_shutdown(int fd, int how);
#define getsockopt fake_getsockopt
#define setsockopt fake_setsockopt
#define shutdown fake_shutdown
