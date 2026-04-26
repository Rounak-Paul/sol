// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_file.c — see header.

#include "sol_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *sol_file_result_str(SolFileResult r)
{
    switch (r) {
    case SOL_FILE_OK:            return "ok";
    case SOL_FILE_ERR_OPEN:      return "open failed";
    case SOL_FILE_ERR_READ:      return "read failed";
    case SOL_FILE_ERR_WRITE:     return "write failed";
    case SOL_FILE_ERR_RENAME:    return "rename failed";
    case SOL_FILE_ERR_OOM:       return "out of memory";
    case SOL_FILE_ERR_TOO_LARGE: return "file exceeds maximum size";
    }
    return "unknown";
}

SolFileResult sol_file_read_all(const char *path,
                                size_t      max_bytes,
                                char      **out_data,
                                size_t     *out_length)
{
    if (!path || !out_data || !out_length) return SOL_FILE_ERR_OPEN;
    *out_data   = NULL;
    *out_length = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return SOL_FILE_ERR_OPEN;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return SOL_FILE_ERR_READ; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return SOL_FILE_ERR_READ; }
    if (max_bytes && (size_t)sz > max_bytes) { fclose(fp); return SOL_FILE_ERR_TOO_LARGE; }
    rewind(fp);

    char *data = (char *)malloc((size_t)sz + 1);
    if (!data) { fclose(fp); return SOL_FILE_ERR_OOM; }

    size_t n = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) { free(data); return SOL_FILE_ERR_READ; }

    data[sz] = '\0';
    *out_data   = data;
    *out_length = (size_t)sz;
    return SOL_FILE_OK;
}

SolFileResult sol_file_write_all_atomic(const char *path,
                                        const void *data,
                                        size_t      length)
{
    if (!path || (!data && length > 0)) return SOL_FILE_ERR_OPEN;

    char tmp[4096];
    int  printed = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    if (printed < 0 || printed >= (int)sizeof(tmp)) return SOL_FILE_ERR_OPEN;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return SOL_FILE_ERR_OPEN;

    const unsigned char *p = (const unsigned char *)data;
    size_t left = length;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return SOL_FILE_ERR_WRITE;
        }
        p    += (size_t)w;
        left -= (size_t)w;
    }

    /* Best-effort durability. fsync may fail on some filesystems; we
       don't treat that as fatal because the rename below still gives
       atomicity vs. concurrent readers. */
    (void)fsync(fd);
    close(fd);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return SOL_FILE_ERR_RENAME;
    }
    return SOL_FILE_OK;
}
