// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_text_buffer.c — Implementation. */

#include "sol_text_buffer.h"

#include <stdlib.h>
#include <string.h>

typedef struct SolTextBufferState {
    SolRope *rope;
} SolTextBufferState;

static void text_buffer_destroy(void *state)
{
    SolTextBufferState *s = (SolTextBufferState *)state;
    if (!s) return;
    sol_rope_destroy(s->rope);
    free(s);
}

static void text_buffer_render(const SolBuffer *buffer,
                               const SolBufferRenderArgs *args, void *state)
{
    /* Rendering is owned by the UI layer; sol's UI does not yet have a
       rope-aware viewer. Until then this is a deliberate no-op so the
       buffer can still be opened, focused, and split. */
    (void)buffer;
    (void)args;
    (void)state;
}

static const char *basename_of(const char *path)
{
    if (!path) return "untitled";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

SolBufferId sol_text_buffer_open_empty(SolBufferSystem *system,
                                       const char *display_name)
{
    if (!system) return 0u;

    SolTextBufferState *state = (SolTextBufferState *)malloc(sizeof(*state));
    if (!state) return 0u;
    state->rope = sol_rope_create();
    if (!state->rope) { free(state); return 0u; }

    SolBufferDesc desc = {
        .name = display_name ? display_name : "untitled",
        .kind = SOL_BUFFER_KIND_TEXT,
        .state = state,
        .ops   = {
            .destroy = text_buffer_destroy,
            .render  = text_buffer_render,
        },
    };
    SolBufferId id = sol_buffer_create(system, &desc);
    if (id == 0u) {
        text_buffer_destroy(state);
    }
    return id;
}

SolBufferId sol_text_buffer_open_file(SolBufferSystem *system,
                                      const char *path,
                                      const char *display_name,
                                      const char **out_error)
{
    if (!system || !path) {
        if (out_error) *out_error = "null argument";
        return 0u;
    }

    const char *err = NULL;
    SolRope *rope = sol_rope_from_file(path, &err);
    if (!rope) {
        if (out_error) *out_error = err ? err : "failed to load file";
        return 0u;
    }

    SolTextBufferState *state = (SolTextBufferState *)malloc(sizeof(*state));
    if (!state) {
        sol_rope_destroy(rope);
        if (out_error) *out_error = "out of memory";
        return 0u;
    }
    state->rope = rope;

    SolBufferDesc desc = {
        .name = display_name ? display_name : basename_of(path),
        .kind = SOL_BUFFER_KIND_TEXT,
        .state = state,
        .ops   = {
            .destroy = text_buffer_destroy,
            .render  = text_buffer_render,
        },
    };
    SolBufferId id = sol_buffer_create(system, &desc);
    if (id == 0u) {
        text_buffer_destroy(state);
        if (out_error) *out_error = "buffer system rejected the buffer";
        return 0u;
    }
    return id;
}

SolRope *sol_text_buffer_rope(SolBuffer *buffer)
{
    if (!buffer) return NULL;
    if (sol_buffer_kind(buffer) != SOL_BUFFER_KIND_TEXT) return NULL;
    SolTextBufferState *s = (SolTextBufferState *)sol_buffer_state(buffer);
    return s ? s->rope : NULL;
}
