// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_text_view.h — Causality rendering for SolTextBuffer.
 *
 * The text view is the SolBufferRenderFn that the buffer-system calls
 * during causality reconcile. It walks the rope's visible lines and
 * emits the gutter / text / caret / scrollbar widgets.
 *
 * The view also owns the click + drag handler that converts pointer
 * coordinates into a cursor position on the underlying buffer.
 *
 * No public state — everything reads from the SolTextBuffer the
 * buffer-system passes through as `state`.
 */

#ifndef SOL_TEXT_VIEW_H
#define SOL_TEXT_VIEW_H

#include "sol_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SolBufferRenderFn-compatible. Pass this as `render` to any of the
   sol_text_buffer_open_* constructors. */
void sol_text_view_render(const SolBuffer *buffer,
                          const SolBufferRenderArgs *args,
                          void *state);

/* Number of lines that fit in the viewport for a given window height.
   Used by the input router to clamp scroll deltas and ensure-cursor-
   visible math without poking the view's internal constants. */
int sol_text_view_visible_lines(int window_h);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TEXT_VIEW_H */
