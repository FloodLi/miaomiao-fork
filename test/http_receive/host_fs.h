#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

extern const char *host_fs_root;
extern int host_open_files;
extern bool host_fail_write, host_fail_close;
extern int host_write_calls, host_fail_write_call;
extern const char *host_fail_remove_path;
FILE *host_fopen(const char *path, const char *mode);
int host_fclose(FILE *file);
size_t host_fwrite(const void *data, size_t size, size_t count, FILE *file);
int host_stat(const char *path, struct stat *st);
int host_mkdir(const char *path, mode_t mode);
int host_remove(const char *path);
int host_unlink(const char *path);
int host_rename(const char *from, const char *to);
int host_access(const char *path, int mode);

#ifndef HTTP_HOST_FS_IMPLEMENTATION
#define fopen host_fopen
#define fclose host_fclose
#define fwrite host_fwrite
#define stat(path, st) host_stat(path, st)
#define mkdir host_mkdir
#define remove host_remove
#define unlink host_unlink
#define rename host_rename
#define access host_access
#endif
