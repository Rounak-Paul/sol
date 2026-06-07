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

typedef struct SolUISystem SolUISystem;
typedef struct SolTextBuffer SolTextBuffer;

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
int sol_text_view_visible_lines(int window_h, float ui_scale);

/* Number of lines that fit in the viewport for a pane height in CSS px.
   This is the geometry-aware variant used by split buffer panes. */
int sol_text_view_visible_lines_for_height(float pane_h, float ui_scale);

/* Number of visual monospace columns that fit in the text viewport for
   a pane width in CSS px. `glyph_advance_layout_px` is the measured
   monospace advance in layout pixels; pass 0 to use the fallback. */
int sol_text_view_visible_cols_for_width(float pane_w, float ui_scale,
                                         float glyph_advance_layout_px);

/* Convert text-view-local coordinates into a buffer line and codepoint column.
   `ui` supplies current scale/window metrics; `tb` supplies scroll offsets. */
bool sol_text_view_local_point_to_line_col(SolUISystem *ui,
                                           SolTextBuffer *tb,
                                           float local_x,
                                           float local_y,
                                           size_t *out_line,
                                           size_t *out_cp_col);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TEXT_VIEW_H */
