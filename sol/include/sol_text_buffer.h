// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_text_buffer.h — Text-kind buffers backed by a rope.
 *
 * A thin wrapper around sol_rope that registers itself with sol_buffer
 * as SOL_BUFFER_KIND_TEXT. Use sol_text_buffer_open_file to open a
 * file lazily via mmap; the rope retains the mapping until the buffer
 * is closed.
 */

#ifndef SOL_TEXT_BUFFER_H
#define SOL_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

#include "sol_buffer.h"
#include "sol_rope.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open a file-backed text buffer. On success returns the buffer id;
   on failure returns 0 and (if out_error is non-NULL) writes a
   borrowed error string. Caller-supplied display name is optional —
   pass NULL to derive the name from the file path. */
SolBufferId sol_text_buffer_open_file(SolBufferSystem *system,
                                      const char *path,
                                      const char *display_name,
                                      const char **out_error);

/* Open an empty text buffer with the given display name. */
SolBufferId sol_text_buffer_open_empty(SolBufferSystem *system,
                                       const char *display_name);

/* Borrow the underlying rope from a TEXT buffer. Returns NULL if the
   buffer is not text-kind or is not registered. */
SolRope *sol_text_buffer_rope(SolBuffer *buffer);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TEXT_BUFFER_H */
