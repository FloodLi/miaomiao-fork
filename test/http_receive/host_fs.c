#define HTTP_HOST_FS_IMPLEMENTATION
#include "host_fs.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

const char *host_fs_root;
int host_open_files;
bool host_fail_write, host_fail_close;
int host_write_calls, host_fail_write_call;
const char *host_fail_remove_path;

static const char *mapped(const char *path, char out[4096])
{
    if (strncmp(path, "/spiffs", 7) != 0 || (path[7] && path[7] != '/')) return path;
    assert(host_fs_root);
    int length = snprintf(out, 4096, "%s%s", host_fs_root, path + 7);
    assert(length >= 0 && length < 4096);
    return out;
}

FILE *host_fopen(const char *path, const char *mode)
{
    char buf[4096];
    FILE *file = fopen(mapped(path, buf), mode);
    if (file) host_open_files++;
    return file;
}

int host_fclose(FILE *file)
{
    int result = fclose(file);
    assert(host_open_files > 0);
    host_open_files--;
    if (host_fail_close) { errno = EIO; return EOF; }
    return result;
}

size_t host_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (++host_write_calls == host_fail_write_call || host_fail_write) {
        errno = ENOSPC;
        return 0;
    }
    return fwrite(data, size, count, file);
}

int host_stat(const char *path, struct stat *st)
{
    char buf[4096];
    return stat(mapped(path, buf), st);
}

int host_mkdir(const char *path, mode_t mode)
{
    char buf[4096];
    return mkdir(mapped(path, buf), mode);
}

int host_remove(const char *path)
{
    if (host_fail_remove_path && strcmp(path, host_fail_remove_path) == 0) { errno = EIO; return -1; }
    char buf[4096];
    return remove(mapped(path, buf));
}

int host_unlink(const char *path)
{
    char buf[4096];
    return unlink(mapped(path, buf));
}

int host_rename(const char *from, const char *to)
{
    char src[4096], dst[4096];
    /* Match SPIFFS: promotion cannot overwrite an existing destination. */
    const char *target = mapped(to, dst);
    if (access(target, F_OK) == 0) { errno = EEXIST; return -1; }
    return rename(mapped(from, src), target);
}

int host_access(const char *path, int mode)
{
    char buf[4096];
    return access(mapped(path, buf), mode);
}
