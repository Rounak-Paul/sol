// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_input_router.h — Wire causality input events into Sol.
 *
 * The router owns:
 *
 *   - The seven ca_event_set_handler hookups (KEY / CHAR / mouse /
 *     window close & resize).
 *   - The latest cursor position cache used to route MOUSE_SCROLL to
 *     the pane under the pointer.
 *   - The translation of causality keys into the buffer's editing
 *     primitives (Arrow / Home / End / Backspace / Delete / Enter +
 *     printable codepoints).
 *
 * Construct AFTER the UI system, input system, and buffer system are
 * up. Destroy BEFORE any of them. The router does not own those
 * subsystems — it only borrows them.
 */

#ifndef SOL_INPUT_ROUTER_H
#define SOL_INPUT_ROUTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ca_Instance     Ca_Instance;
typedef struct SolUISystem     SolUISystem;
typedef struct SolInputSystem  SolInputSystem;
typedef struct SolBufferSystem SolBufferSystem;

typedef struct SolInputRouter SolInputRouter;

/*
 * Create the input router and install all causality event handlers.
 *
 * Construct AFTER the UI system, input system, and buffer system are up.
 * The router borrows all four objects — it does not own them.
 *
 * instance  The causality instance whose event hooks to install.
 * ui        The UI system (receives leader-key and resize events).
 * input     The input system (receives translated SolInputEvents).
 * buffers   The buffer system (used for text editing dispatch).
 * Returns   A heap-allocated router, or NULL on failure.
 */
SolInputRouter *sol_input_router_create(Ca_Instance     *instance,
                                        SolUISystem     *ui,
                                        SolInputSystem  *input,
                                        SolBufferSystem *buffers);

/*
 * Destroy the input router and uninstall all causality event handlers.
 *
 * Call this BEFORE destroying any of the subsystems it was created with.
 */
void sol_input_router_destroy(SolInputRouter *router);

/*
 * Force whether the buffer text-editing area accepts keyboard input,
 * independent of the last mouse click. Used by command actions (e.g.
 * "return focus to the buffer") that move keyboard focus back to the
 * buffer panel programmatically rather than via a click the router
 * would otherwise observe through CA_EVENT_MOUSE_BUTTON.
 *
 * router  The router to update.
 * active  True to route subsequent key/char events into the active
 *         text buffer; false to stop doing so.
 */
void sol_input_router_set_buffer_input_active(SolInputRouter *router, bool active);

#ifdef __cplusplus
}
#endif

#endif /* SOL_INPUT_ROUTER_H */
