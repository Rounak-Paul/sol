// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_file.h — minimal, robust file I/O for the editor.
//
// Reads return malloc'd buffers (caller frees). Writes are atomic:
// data goes to a temp file first and is then renamed over the target,
// matching what production editors do to avoid corruption on crash
// or power-loss mid-write.

#ifndef SOL_FILE_H
#define SOL_FILE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SolFileResult {
    SOL_FILE_OK = 0,
    SOL_FILE_ERR_OPEN,
    SOL_FILE_ERR_READ,
    SOL_FILE_ERR_WRITE,
    SOL_FILE_ERR_RENAME,
    SOL_FILE_ERR_OOM,
    SOL_FILE_ERR_TOO_LARGE,
} SolFileResult;

/* Read the entire file into a freshly-allocated, NUL-terminated buffer.
   `*out_data` receives the malloc'd pointer (caller frees with free).
   `*out_length` receives the byte length excluding the trailing NUL.
   Files larger than `max_bytes` (0 = unlimited) are rejected. */
SolFileResult sol_file_read_all(const char *path,
                                size_t      max_bytes,
                                char      **out_data,
                                size_t     *out_length);

/* Write `length` bytes to `path` atomically: write to "<path>.tmp.<pid>",
   fsync, then rename. */
SolFileResult sol_file_write_all_atomic(const char *path,
                                        const void *data,
                                        size_t      length);

const char *sol_file_result_str(SolFileResult r);

#ifdef __cplusplus
}
#endif

#endif
