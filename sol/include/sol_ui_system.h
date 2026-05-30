#ifndef SOL_UI_SYSTEM_H
#define SOL_UI_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>

#include <causality.h>

#include "sol_buffer.h"
#include "sol_input.h"

typedef struct SolUISystem SolUISystem;

/* Maximum number of chord steps in a single command flow (including
 * the leader). Surfaced publicly so callers (e.g. the bindings config
 * loader) can size their per-flow buffers without depending on the
 * private internal header. */
#define SOL_UI_MAX_FLOW_SEQUENCE_LEN 8u

/* Callback invoked when the user clicks a file row in the tree panel.
 * Sol's main wires this to a buffer-create + focus path. Return true on
 * success — the UI ignores the value today but will surface it later. */
typedef bool (*SolUIFileOpenFn)(const char *path, void *user_data);
typedef void (*SolUIFocusRegionFn)(bool in_explorer, void *user_data);

typedef struct SolCommandFlowDesc {
	const char *action;
	const char *label;
	const SolKeyCode *sequence;
	/* Optional parallel array of per-step modifier masks. NULL means
	 * every step has no modifier (the leader modifier is implicit and
	 * MUST NOT be included here). Only Shift / Alt / Super are
	 * meaningful — Ctrl is the leader and is consumed by the popup. */
	const SolModifierMask *step_modifiers;
	size_t sequence_length;
	SolKeyCode key;
	SolInputActionCallback callback;
	void *user_data;
} SolCommandFlowDesc;

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers);
void sol_ui_system_destroy(SolUISystem *ui);

/* Called once per frame BEFORE ca_instance_tick. Polls the file-tree
 * scroll offset and pushes it into sig_tree_scroll so the sticky-ancestor
 * overlay updates in sync with native scroll without rebuilding the
 * workspace content tree. */
void sol_ui_system_pre_tick(SolUISystem *ui);

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui);

bool sol_ui_system_register_command_flow(SolUISystem *ui, const SolCommandFlowDesc *desc);
bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event);

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window);
void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height);

/* File tree integration. Calling set_file_tree_root with a directory
 * path mounts the left-side hierarchy panel. Pass NULL to hide it. */
bool sol_ui_system_set_file_tree_root(SolUISystem *ui, const char *path);
void sol_ui_system_set_file_open_callback(SolUISystem *ui,
                                          SolUIFileOpenFn callback,
                                          void *user_data);

/* Receive focus-region transitions from concrete UI interactions
 * (tree rows / pane clicks). in_explorer=true means interaction inside
 * explorer; false means interaction in buffer/workspace content. */
void sol_ui_system_set_focus_region_callback(SolUISystem *ui,
											 SolUIFocusRegionFn callback,
											 void *user_data);

/* Force the buffer split-tree area to rebuild on the next reactive
 * flush. Call this after externally swapping the active buffer (e.g.
 * from a file-picker callback) so the new contents render immediately
 * instead of waiting for an unrelated invalidation. */
void sol_ui_system_invalidate_buffer_area(SolUISystem *ui);

/* Set the active workspace leaf and force a rebuild. Returns true if
 * the focused leaf actually changed. Used by buffer-content click
 * handlers (e.g. clicking a line of text) so they don't have to reach
 * into the buffer system directly. */
bool sol_ui_system_focus_leaf(SolUISystem *ui, SolBufferNodeId leaf_id);

/* Last-known logical window size in CSS pixels. Either out param may be
 * NULL. Both are 0 until the first resize callback fires. */
void sol_ui_system_window_size(const SolUISystem *ui, int *out_w, int *out_h);

/* Geometry of the chrome strips around the buffer area, in CSS pixels.
 * Used by the host application to do its own hit-testing (e.g. routing
 * mouse-wheel events to the pane under the cursor instead of the
 * focused one). */
int  sol_ui_system_title_bar_height(const SolUISystem *ui);
int  sol_ui_system_status_bar_height(const SolUISystem *ui);
int  sol_ui_system_tree_panel_width(const SolUISystem *ui);

/* True while the leader-key popup is visible / a flow chord is being
 * captured. Hosts use this to suppress raw editing input (typing,
 * arrows, backspace, …) while a flow is in progress, so e.g. pressing
 * Ctrl+W,V doesn't also insert characters into the active buffer. */
bool sol_ui_system_is_leader_active(const SolUISystem *ui);

/* Title-bar menu integration. The callbacks fire when the user
 * picks items from the "File" menu or clicks welcome-screen buttons.
 * Pass NULL for any to leave that item disabled. The menu is
 * (re)installed on the primary window's title bar each call. */
typedef void (*SolUIMenuActionFn)(void *user_data);
void sol_ui_system_install_menu(SolUISystem      *ui,
                                SolUIMenuActionFn on_new_buffer,
                                SolUIMenuActionFn on_open_file,
                                SolUIMenuActionFn on_open_folder,
                                void             *user_data);

/* Per-frame tick — drives async UI work owned by the UI system
 * (currently: reaping closed file-picker windows). Safe to call
 * even when nothing async is in flight. */
void sol_ui_system_tick(SolUISystem *ui);

#endif
