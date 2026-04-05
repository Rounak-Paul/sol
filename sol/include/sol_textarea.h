#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Forward-declare opaque types to avoid pulling in full headers. */
typedef struct Sol_Buffer Sol_Buffer;

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   OPAQUE HANDLE
   ============================================================ */

typedef struct Sol_Textarea Sol_Textarea;

/* ============================================================
   LIFECYCLE
   ============================================================ */

/* Allocate a textarea bound to `buf`.
   `init_width` / `init_height` are the initial pixel dimensions of the
   visible editing area (used for virtual-scroll computation).  Update
   them with sol_textarea_set_size() when the window resizes. */
Sol_Textarea *sol_textarea_create(Sol_Buffer *buf,
                                  float init_width, float init_height);

void sol_textarea_destroy(Sol_Textarea *tv);

/* ============================================================
   BUFFER BINDING
   ============================================================ */

void       sol_textarea_set_buffer(Sol_Textarea *tv, Sol_Buffer *buf);
Sol_Buffer *sol_textarea_buffer(const Sol_Textarea *tv);

/* ============================================================
   SIZE
   ============================================================ */

/* Notify the textarea that the visible area has changed (e.g. on window resize). */
void sol_textarea_set_size(Sol_Textarea *tv, float width, float height);

/* ============================================================
   CAUSALITY INTEGRATION
   ============================================================ */

/* Call ONCE inside ca_ui_begin / ca_ui_end to plant the skeleton outer
   container node.  Stores a handle to it for per-frame rebuilds.
   Width/height of 0 means "fill parent". */
void sol_textarea_build(Sol_Textarea *tv, float width, float height);

/* Call ONCE per frame inside the ca_window_set_on_frame callback.
   Clears the outer div and rebuilds only the visible lines. */
void sol_textarea_update(Sol_Textarea *tv);

/* ============================================================
   FOCUS
   ============================================================ */

void sol_textarea_focus(Sol_Textarea *tv);
void sol_textarea_blur(Sol_Textarea *tv);
bool sol_textarea_is_focused(const Sol_Textarea *tv);

/* ============================================================
   SCROLL
   ============================================================ */

/* Ensure the cursor line is visible; adjust scroll_y if needed. */
void sol_textarea_scroll_to_cursor(Sol_Textarea *tv);

#ifdef __cplusplus
}
#endif
