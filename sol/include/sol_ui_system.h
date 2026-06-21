#ifndef SOL_UI_SYSTEM_H
#define SOL_UI_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>

#include <causality.h>

#include "sol_buffer.h"
#include "sol_input.h"
#include "sol_theme.h"

typedef struct SolUISystem SolUISystem;

/* Height reserved by each buffer leaf for its pane-local tab strip. */
#define SOL_UI_BUFFER_TAB_STRIP_HEIGHT 22.0f

typedef uint32_t SolUISidePanelToken;
#define SOL_UI_SIDE_PANEL_TOKEN_INVALID 0u
typedef uint32_t SolUIMenuItemToken;
#define SOL_UI_MENU_ITEM_TOKEN_INVALID 0u

/* Render callback for a plugin-contributed workspace side panel. */
typedef void (*SolUISidePanelRenderFn)(void *user_data);

/* Per-frame callback for adopting asynchronous panel state on the UI thread. */
typedef void (*SolUISidePanelTickFn)(void *user_data);

/* Descriptor for a plugin-contributed workspace side panel. */
typedef struct SolUISidePanelDesc {
    const char              *id;
    const char              *title;
    SolUISidePanelRenderFn   render;
    SolUISidePanelTickFn     tick;
    void                    *user_data;
} SolUISidePanelDesc;

/* Descriptor for a command-backed title-bar menu item. */
typedef struct SolUIMenuItemDesc {
    const char *menu_id;       /* stable menu identifier                   */
    const char *menu_label;    /* visible top-level menu label             */
    const char *item_id;       /* stable identifier unique within menu     */
    const char *label;         /* visible menu-item label                  */
    const char *action;        /* command action invoked on click          */
    const char *submenu_id;    /* optional stable submenu identifier        */
    const char *submenu_label; /* optional visible submenu label            */
    int         menu_order;    /* top-level ordering for newly-created menu */
    int         item_order;    /* ordering within the target menu          */
} SolUIMenuItemDesc;

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
/* Fired just before terminal keyboard focus is granted so the app can
   snapshot whatever held focus beforehand and restore it on defocus. */
typedef void (*SolUITerminalFocusGainFn)(void *user_data);

typedef enum SolUIContextSurface {
    SOL_UI_CONTEXT_SURFACE_WORKSPACE = 0,
    SOL_UI_CONTEXT_SURFACE_EXPLORER_ROOT,
    SOL_UI_CONTEXT_SURFACE_EXPLORER_ITEM,
    SOL_UI_CONTEXT_SURFACE_EXPLORER_EMPTY,
    SOL_UI_CONTEXT_SURFACE_BUFFER_TEXT,
    SOL_UI_CONTEXT_SURFACE_BUFFER_BODY,
    SOL_UI_CONTEXT_SURFACE_BUFFER_TAB,
} SolUIContextSurface;

typedef enum SolUIContextAction {
    SOL_UI_CONTEXT_ACTION_NONE = 0,
    SOL_UI_CONTEXT_ACTION_OPEN,
    SOL_UI_CONTEXT_ACTION_OPEN_FILE_PICKER,
    SOL_UI_CONTEXT_ACTION_OPEN_FOLDER_PICKER,
    SOL_UI_CONTEXT_ACTION_NEW_BUFFER,
    SOL_UI_CONTEXT_ACTION_CLOSE_BUFFER,
    SOL_UI_CONTEXT_ACTION_CLOSE_TAB,
    SOL_UI_CONTEXT_ACTION_CLOSE_ALL_BUFFERS,
    SOL_UI_CONTEXT_ACTION_SPLIT_VERTICAL,
    SOL_UI_CONTEXT_ACTION_SPLIT_HORIZONTAL,
    SOL_UI_CONTEXT_ACTION_NEW_FILE,
    SOL_UI_CONTEXT_ACTION_NEW_FOLDER,
    SOL_UI_CONTEXT_ACTION_DELETE_PATH,
    SOL_UI_CONTEXT_ACTION_COPY_PATH,
    SOL_UI_CONTEXT_ACTION_CUT_PATH,
    SOL_UI_CONTEXT_ACTION_PASTE_PATH,
    SOL_UI_CONTEXT_ACTION_COPY_TEXT,
    SOL_UI_CONTEXT_ACTION_COPY_LINE,
    SOL_UI_CONTEXT_ACTION_CUT_TEXT,
    SOL_UI_CONTEXT_ACTION_PASTE_TEXT,
    SOL_UI_CONTEXT_ACTION_PASTE_LINE,
    SOL_UI_CONTEXT_ACTION_SELECT_ALL_TEXT,
    SOL_UI_CONTEXT_ACTION_DELETE_TEXT,
    SOL_UI_CONTEXT_ACTION_DELETE_LINE,
} SolUIContextAction;

typedef struct SolUIContextActionRequest {
    SolUIContextAction action;
    SolUIContextSurface surface;
    const char *path;
    bool path_is_dir;
    SolBufferNodeId leaf_id;
    SolBufferId buffer_id;
    bool has_local_point;
    float local_x;
    float local_y;
    float screen_x;
    float screen_y;
} SolUIContextActionRequest;

typedef bool (*SolUIContextActionFn)(const SolUIContextActionRequest *request,
                                     void *user_data);

typedef struct SolCommandFlowDesc {
	const char *action;
	const char *label;
	const SolKeyCode *sequence;
	/* Optional parallel array of per-step modifier masks. NULL means
	 * every step has no modifier (the leader modifier is implicit and
	 * MUST NOT be included here). Only the non-leader modifiers are
	 * meaningful at each step — the leader modifier is consumed by the
	 * popup and stripped before matching. */
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

/* Unregister a previously-registered command flow by action string.
 * Returns true if found and removed, false if not found.              */
bool sol_ui_system_unregister_command_flow(SolUISystem *ui, const char *action);
/* Invoke an action through the registered callback and command event bus. */
bool sol_ui_system_invoke_command(SolUISystem *ui, const char *action);
bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event);

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window);
void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height);

/* File tree integration. Calling set_file_tree_root with a directory
 * path mounts the left-side hierarchy panel. Pass NULL to hide it. */
bool sol_ui_system_set_file_tree_root(SolUISystem *ui, const char *path);
void sol_ui_system_set_file_tree_visible(SolUISystem *ui, bool visible);
bool sol_ui_system_file_tree_visible(const SolUISystem *ui);
/* Select the built-in file tree as the current left-sidebar content. */
void sol_ui_system_show_file_tree(SolUISystem *ui);
/* Return whether the file tree currently owns the visible left sidebar. */
bool sol_ui_system_file_tree_active(const SolUISystem *ui);
const char *sol_ui_system_file_tree_root(const SolUISystem *ui);
void sol_ui_system_set_file_open_callback(SolUISystem *ui,
                                          SolUIFileOpenFn callback,
                                          void *user_data);

/* Receive focus-region transitions from concrete UI interactions
 * (tree rows / pane clicks). in_explorer=true means interaction inside
 * explorer; false means interaction in buffer/workspace content. */
void sol_ui_system_set_focus_region_callback(SolUISystem *ui,
											 SolUIFocusRegionFn callback,
											 void *user_data);

/* Register a callback fired just before terminal keyboard focus is granted.
 * The app uses this to snapshot pre-terminal focus state for later restore. */
void sol_ui_system_set_terminal_focus_gain_callback(SolUISystem *ui,
                                                    SolUITerminalFocusGainFn callback,
                                                    void *user_data);

/* Install the callback used by Causality-backed context menus.
 * The UI only reports typed target/action requests; the application
 * owns command dispatch and filesystem mutation policy. */
void sol_ui_system_set_context_action_callback(SolUISystem *ui,
                                               SolUIContextActionFn callback,
                                               void *user_data);

/* Force the buffer split-tree area to rebuild on the next reactive
 * flush. Call this after externally swapping the active buffer (e.g.
 * from a file-picker callback) so the new contents render immediately
 * instead of waiting for an unrelated invalidation. */
void sol_ui_system_invalidate_buffer_area(SolUISystem *ui);

/* Register a workspace side panel and return its stable token. */
SolUISidePanelToken sol_ui_system_register_side_panel(
    SolUISystem *ui,
    const SolUISidePanelDesc *desc);

/* Remove a side-panel contribution. The active panel is hidden if it matches. */
void sol_ui_system_unregister_side_panel(SolUISystem *ui,
                                         SolUISidePanelToken token);

/* Show a registered side panel, replacing the explorer until hidden. */
bool sol_ui_system_show_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token);

/* Hide a registered side panel and return the sidebar to the explorer. */
void sol_ui_system_hide_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token);

/* Return whether the token currently owns the visible side panel. */
bool sol_ui_system_side_panel_visible(const SolUISystem *ui,
                                      SolUISidePanelToken token);

/* Notify the reactive workspace that side-panel state changed. */
void sol_ui_system_notify_side_panel(SolUISystem *ui,
                                     SolUISidePanelToken token);

/* Wake the Causality event loop after a worker publishes panel state. */
void sol_ui_system_wake(SolUISystem *ui);

/* Set the active workspace leaf and force a rebuild. Returns true if
 * the focused leaf actually changed. Used by buffer-content click
 * handlers (e.g. clicking a line of text) so they don't have to reach
 * into the buffer system directly. */
bool sol_ui_system_focus_leaf(SolUISystem *ui, SolBufferNodeId leaf_id);

/* Last-known logical window size in CSS pixels. Either out param may be
 * NULL. Both are 0 until the first resize callback fires. */
void sol_ui_system_window_size(const SolUISystem *ui, int *out_w, int *out_h);

/* Root rectangle of the buffer split tree inside the workspace area.
 * This excludes the title bar, status bar, and the global buffer tab row.
 * Returns false when the UI is not ready or the workspace is collapsed. */
bool sol_ui_system_buffer_area_rect(const SolUISystem *ui,
                                    float *out_x,
                                    float *out_y,
                                    float *out_w,
                                    float *out_h);

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
 * L,W,V doesn't also insert characters into the active buffer. */
bool sol_ui_system_is_leader_active(const SolUISystem *ui);

/* Get / set the leader modifier. The leader is the modifier key that
 * opens the command-flow popup (default: SOL_MOD_CTRL). Changing it
 * takes effect immediately; call before registering flows so that the
 * leader-stripping pass in sol_ui_system_register_command_flow uses the
 * correct mask. Typically called by the config loader when it processes
 * the `leader` directive. */
SolModifierMask sol_ui_system_leader_modifier(const SolUISystem *ui);
void            sol_ui_system_set_leader_modifier(SolUISystem *ui,
                                                  SolModifierMask mod);

void sol_ui_system_attach_buffer_text_context_menu(SolUISystem *ui,
                                                   SolBufferNodeId leaf_id,
                                                   SolBufferId buffer_id);

/* Title-bar menu integration. The callbacks back host-owned File menu and
 * welcome-screen actions. Dynamic command items are registered separately. */
typedef void (*SolUIMenuActionFn)(void *user_data);
void sol_ui_system_install_menu(SolUISystem      *ui,
                                SolUIMenuActionFn on_new_buffer,
                                SolUIMenuActionFn on_open_file,
                                SolUIMenuActionFn on_open_folder,
                                void             *user_data);
/* Register a command-backed item in an existing or new top-level menu. */
SolUIMenuItemToken sol_ui_system_register_menu_item(
    SolUISystem *ui,
    const SolUIMenuItemDesc *desc);
/* Remove a previously registered title-bar menu item. */
void sol_ui_system_unregister_menu_item(SolUISystem *ui,
                                        SolUIMenuItemToken token);

/* Per-frame tick — drives async UI work owned by the UI system
 * (currently: reaping closed file-picker windows). Safe to call
 * even when nothing async is in flight. */
void sol_ui_system_tick(SolUISystem *ui);

/* ================================================================== */
/* Plugin Manager window                                               */
/* ================================================================== */

/* Associate a SolPluginManager with the UI system so the plugin
 * window can read plugin state. Called from sol_plugin_manager_attach_ui
 * (via workspace.c) — application code does not need to call this
 * directly.                                                           */
typedef struct SolPluginManager SolPluginManager;
void sol_ui_system_set_plugin_manager(SolUISystem    *ui,
                                       SolPluginManager *pm);

/* Open the plugin manager overlay window. */
void sol_ui_system_open_plugin_window(SolUISystem *ui);

/* ================================================================== */
/* Settings                                                            */
/* ================================================================== */

/* Forward declaration — include sol_settings.h for the full struct. */
typedef struct SolSettings SolSettings;

/* Attach a SolSettings instance.  The pointer must outlive the UI
 * system (typically it lives in the application's stack frame). */
void sol_ui_system_set_settings(SolUISystem *ui, SolSettings *settings);

/* Open the settings window. No-op if one is already open. */
void sol_ui_system_open_settings_window(SolUISystem *ui);

/* Re-generate and apply the stylesheet with the current appearance overlay
 * from the attached SolSettings.  Call after modifying any appearance field
 * in settings to make the change take effect immediately. */
void sol_ui_system_apply_appearance(SolUISystem *ui);

/* Register a complete CSS theme after validating that it parses. */
bool sol_ui_system_register_theme(SolUISystem *ui, const SolThemeDesc *desc);

/* Remove a registered non-default theme. */
bool sol_ui_system_unregister_theme(SolUISystem *ui, const char *id);

/* Select a registered theme and apply it to every live window. */
bool sol_ui_system_set_active_theme(SolUISystem *ui, const char *id);

/* Return the active theme id, or NULL when unavailable. */
const char *sol_ui_system_active_theme(const SolUISystem *ui);

/* Enumerate registered themes. Returned strings remain registry-owned. */
size_t sol_ui_system_theme_count(const SolUISystem *ui);
bool sol_ui_system_theme_info(const SolUISystem *ui, size_t index,
                              const char **out_id, const char **out_name);

/* ================================================================== */
/* Background effect registry integration                             */
/* ================================================================== */

typedef struct SolBgEffectRegistry SolBgEffectRegistry;

/*
 * Attach a background effect registry to the UI system.
 * The registry's viewport is connected to the background builder so shader
 * effects render behind all UI content.  Pass NULL to detach.
 *
 * ui   The UI system.
 * reg  Effect registry (may be NULL).
 */
void sol_ui_system_set_bg_effects(SolUISystem *ui, SolBgEffectRegistry *reg);

/*
 * Return the attached background effect registry, or NULL if none.
 *
 * ui  The UI system.
 */
SolBgEffectRegistry *sol_ui_system_bg_effects(const SolUISystem *ui);

/* Workspace search windows. File search fuzzy-matches relative paths;
 * content search finds matching lines across the mounted tree root. */
void sol_ui_system_open_file_search(SolUISystem *ui);
void sol_ui_system_open_content_search(SolUISystem *ui);

/* ================================================================== */
/* Terminal manager integration                                        */
/* ================================================================== */

typedef struct SolTerminalManager SolTerminalManager;

/*
 * Attach a terminal manager to the UI system so the workspace can render it.
 *
 * ui   The UI system.
 * mgr  The terminal manager to attach (may be NULL to detach).
 */
void sol_ui_system_set_terminal_manager(SolUISystem *ui, SolTerminalManager *mgr);

/*
 * Return the attached terminal manager, or NULL if none is attached.
 *
 * ui  The UI system.
 */
SolTerminalManager *sol_ui_system_terminal_manager(const SolUISystem *ui);

/*
 * Bump the terminal revision signal so the workspace redraws the terminal panel.
 * Call after any terminal state change that the renderer must reflect.
 *
 * ui  The UI system.
 */
void sol_ui_system_terminal_notify(SolUISystem *ui);

/*
 * Set terminal keyboard focus, firing the focus-gain callback when gaining
 * focus so the application can snapshot pre-terminal focus state.
 * Use this instead of calling sol_terminal_manager_set_focused directly.
 *
 * ui      The UI system.
 * focused Whether the terminal should have keyboard focus.
 */
void sol_ui_system_terminal_set_focused(SolUISystem *ui, bool focused);

/* ================================================================== */
/* Plugin status bar segments                                          */
/*                                                                     */
/* Plugins contribute text segments shown on the RIGHT side of the     */
/* status bar.  Use sol_plugin_add/update/remove_status_segment from   */
/* sol_plugin_ctx.h; these lower-level functions are for the plugin    */
/* context implementation.                                             */
/* ================================================================== */

typedef uint32_t SolUIStatusToken;
#define SOL_UI_STATUS_TOKEN_INVALID 0u

SolUIStatusToken sol_ui_system_add_status_segment(
    SolUISystem *ui,
    const char  *text,
    const char  *style_class);   /* CSS class, or NULL for default */

void sol_ui_system_update_status_segment(
    SolUISystem      *ui,
    SolUIStatusToken  token,
    const char       *text);

void sol_ui_system_remove_status_segment(
    SolUISystem      *ui,
    SolUIStatusToken  token);

#endif
