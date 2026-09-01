// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* main.c — Sol entry point.
 *
 * Responsibilities (and only these):
 *
 *   1. Build the bottom-up subsystem stack (systems → causality →
 *      UI system → input router).
 *   2. Parse argv and open the CLI path (file or directory).
 *   3. Wire the title-bar File menu and file-tree click callback.
 *   4. Load key bindings from $HOME/.sol/bindings.conf (auto-seeded
 *      on first launch) and subscribe to the resulting command
 *      events so Sol's built-in actions (split, cycle, focus-last)
 *      respond to them.
 *   5. Drive the frame loop.
 *
 * Everything heavier — text storage, rendering, input dispatch — lives
 * in dedicated modules:
 *
 *   sol/src/core/sol_text_buffer.c   rope-backed editing primitives
 *   sol/src/core/sol_config.c        ~/.sol/bindings.conf loader
 *   sol/src/ui/text_view.c            causality renderer + click/drag
 *   sol/src/ui/input_router.c         causality → Sol event glue
 */

#include <causality.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_buffer.h"
#include "sol_config.h"
#include "sol_event.h"
#include "sol_file_picker.h"
#include "sol_file_watcher.h"
#include "sol_input.h"
#include "sol_input_router.h"
#include "sol_job.h"
#include "sol_platform.h"
#include "sol_bg_effect.h"
#include "sol_settings.h"
#include "sol_system_manager.h"
#include "sol_syntax.h"
#include "sol_terminal.h"
#include "sol_text_buffer.h"
#include "sol_text_view.h"
#include "sol_ui_constants.h"
#include "sol_ui_system.h"

/* Write the directory portion of `path` into `buffer`.
 * Parameters:
 *   path        - source path to split.
 *   buffer      - destination for the null-terminated directory path.
 *   buffer_size - total bytes available in `buffer`.
 */
static bool sol_path_dirname(const char *path, char *buffer, size_t buffer_size)
{
    if (!path || !buffer || buffer_size == 0u) {
        return false;
    }

    const char *sep = NULL;
    for (const char *p = path; *p != '\0'; ++p) {
        if (sol_platform_is_path_separator(*p)) {
            sep = p;
        }
    }
    if (!sep || sep == path) {
        return false;
    }

    const size_t len = (size_t)(sep - path);
    if (len >= buffer_size) {
        return false;
    }
    memcpy(buffer, path, len);
    buffer[len] = '\0';
    return true;
}

/* Resolve the plugin directory located beside the running executable.
 * Parameters:
 *   argv0       - process argv[0], used only if the OS executable path fails.
 *   buffer      - destination for the null-terminated plugin directory path.
 *   buffer_size - total bytes available in `buffer`.
 */
static bool sol_resolve_plugin_directory(const char *argv0,
                                         char       *buffer,
                                         size_t      buffer_size)
{
    if (!buffer || buffer_size == 0u) {
        return false;
    }

    char exe_path[1024];
    char exe_dir[1024];
    bool have_dir = false;

    if (sol_platform_get_executable_path(exe_path, sizeof(exe_path))) {
        have_dir = sol_path_dirname(exe_path, exe_dir, sizeof(exe_dir));
    }
    if (!have_dir && argv0 && argv0[0] != '\0') {
        have_dir = sol_path_dirname(argv0, exe_dir, sizeof(exe_dir));
    }
    if (!have_dir) {
        exe_dir[0] = '.';
        exe_dir[1] = '\0';
    }

    char *joined = sol_platform_path_join(exe_dir, "plugins");
    if (!joined) {
        return false;
    }

    const size_t len = strlen(joined);
    if (len >= buffer_size) {
        free(joined);
        return false;
    }
    memcpy(buffer, joined, len + 1u);
    free(joined);
    return true;
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolAppContext {
    SolSystemManager     *systems;
    SolEventBus          *events;
    SolBufferSystem      *buffers;
    SolJobSystem         *jobs;
    SolInputSystem       *input;
    SolSubscriptionToken  startup_token;
    SolSubscriptionToken  command_token;
    int                   command_flows_loaded;
    SolUISystem          *ui;
    SolSyntaxRegistry    *syntax_registry;
    SolTerminalManager   *terminal_mgr;
    SolBgEffectRegistry  *bg_effects;
    Ca_Instance          *instance;
    SolInputRouter       *router;
    bool                  explorer_focused;
    SolBufferNodeId       focus_before_explorer;
    SolBufferNodeId       focus_before_terminal;
    char                  file_clipboard_path[4096];
    bool                  file_clipboard_cut;
    SolSettings           settings;
    SolSubscriptionToken  text_edited_token;
    /* Autosave debounce: bumped to (edit time + delay) on every text edit
       while autosave is enabled; the frame loop sweeps all dirty buffers
       once this deadline passes. 0 means no autosave is pending. A single
       shared deadline (rather than a per-buffer table) keeps this O(1) to
       maintain — a burst of edits across many buffers just pushes the
       sweep out, and the sweep saves everything dirty at that point. */
    uint64_t              autosave_deadline_ns;
    SolFileWatcher        *watcher;
    /* Status-bar segment showing "external changes not loaded" while at
       least one dirty buffer has a pending external change. Removed once
       no buffer needs the warning anymore. */
    SolUIStatusToken       external_change_status;
    /* Set once sol_run_deferred_init has run. Plugin loading, saved
       theme/background-effect restore, and the startup event all happen
       here instead of before the first frame, so the window paints and
       becomes interactive immediately instead of waiting on disk I/O and
       dynamic-library loading it doesn't need for the first frame. */
    bool                   deferred_init_done;
} SolAppContext;

/* Debounce interval: a dirty buffer is saved this long after its last
   edit, provided no further edit arrives first. Chosen to avoid writing
   to disk on every keystroke while still feeling near-immediate. */
#define SOL_AUTOSAVE_DEBOUNCE_NS 1500000000ull

typedef struct SolDeletePathRequest {
    SolAppContext *app;
    char path[4096];
} SolDeletePathRequest;

/*
 * Trampoline adapting ca_instance_wake() (no arguments) to
 * SolFileWatcherWakeFn's void(*)(void*) signature, so the watcher's
 * background thread can wake the main loop out of glfwWaitEvents()
 * the moment it queues a new event — without this, an idle main loop
 * would only drain the watcher (and thus refresh the explorer / reload
 * buffers) the next time the user happened to move the mouse or type.
 *
 * user_data  Unused; the callback signature carries it for callers that
 *            need per-instance context, which Sol's single Ca_Instance
 *            does not.
 */
static void sol_on_file_watcher_wake(void *user_data)
{
    (void)user_data;
    ca_instance_wake();
}

/* ------------------------------------------------------------------ */
/* Buffer-open glue                                                    */
/* ------------------------------------------------------------------ */

static bool sol_set_explorer_root(SolAppContext *app, const char *path)
{
    if (!app || !app->ui) return false;
    const bool ok = sol_ui_system_set_file_tree_root(app->ui, path);
    if (ok && app->watcher) {
        /* A failed watch (permission denied, OS resource limit, etc.)
           does not fail the explorer-root change — live updates are a
           convenience on top of an otherwise fully-usable explorer. */
        (void)sol_file_watcher_set_root(app->watcher, path);
    }
    return ok;
}

static int sol_active_buffer_viewport_lines(SolAppContext *app, SolTextBuffer *tb)
{
    if (!app || !app->ui || !app->buffers || !tb) {
        return 1;
    }

    const float scale = sol_ui_system_scale(app->ui);
    SolBufferRect root_rect = {0};
    if (sol_ui_system_buffer_area_rect(app->ui, &root_rect.x, &root_rect.y,
                                       &root_rect.w, &root_rect.h)) {
        SolBufferRect leaf_rect = {0};
        const SolBufferNodeId leaf = sol_buffer_active_leaf(app->buffers);
        if (leaf != 0u &&
            sol_buffer_leaf_geometry(app->buffers, leaf, &root_rect,
                                     SOL_UI_PANEL_GAP_PX * scale, &leaf_rect)) {
            leaf_rect.h -= SOL_UI_BUFFER_TAB_STRIP_HEIGHT * scale;
            if (leaf_rect.h < 0.0f) leaf_rect.h = 0.0f;
            int viewport = sol_text_view_visible_lines_for_height(leaf_rect.h, scale) - 2;
            if (viewport < 1) viewport = 1;
            return viewport;
        }
    }

    int win_h = 0;
    sol_ui_system_window_size(app->ui, NULL, &win_h);
    if (win_h <= 0) win_h = 600;
    int viewport = sol_text_view_visible_lines(win_h, scale) - 2;
    if (viewport < 1) viewport = 1;
    return viewport;
}

/* Install defaults before loading bindings.conf. Config entries with the
   same action replace these, so existing custom keymaps remain authoritative
   while older configs still gain newly-added built-in actions. */
static void sol_register_search_command_defaults(SolUISystem *ui)
{
    if (!ui) return;
    const SolKeyCode file_sequence[] = { 'F', 'F' };
    const SolKeyCode content_sequence[] = { 'F', 'G' };
    (void)sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "find.files",
        .label = "Find files",
        .sequence = file_sequence,
        .sequence_length = 2u,
    });
    (void)sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "find.grep",
        .label = "Find in files",
        .sequence = content_sequence,
        .sequence_length = 2u,
    });
}

/*
 * Register default terminal command flows.
 * All flows use 'T' as the second key; final keys select the action.
 *
 * ui  The UI system to register flows with.
 */
static void sol_register_terminal_command_defaults(SolUISystem *ui)
{
    if (!ui) return;
    struct { const char *action; const char *label; char key; } flows[] = {
        { "terminal.toggle",           "Focus/toggle terminal",    'T' },
        { "terminal.position.bottom",  "Terminal: bottom",         'H' },
        { "terminal.position.right",   "Terminal: right",          'V' },
        { "terminal.kill",             "Kill terminal",            'X' },
        { "terminal.tab.new",          "New terminal tab",         'C' },
        { "terminal.tab.next",         "Next terminal tab",        'N' },
        { "terminal.tab.prev",         "Prev terminal tab",        'P' },
    };
    for (size_t i = 0; i < sizeof(flows) / sizeof(flows[0]); ++i) {
        SolKeyCode seq[2] = { 'T', (SolKeyCode)(unsigned char)flows[i].key };
        (void)sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
            .action          = flows[i].action,
            .label           = flows[i].label,
            .sequence        = seq,
            .sequence_length = 2u,
        });
    }
}

/*
 * Register default save command flows.
 *
 * Registered in code (not just via bindings.conf) so leader-b-s /
 * leader-b-shift+s work immediately even for users with a pre-existing
 * bindings.conf from before these actions existed — that file is only
 * auto-seeded with new defaults when it doesn't exist yet.
 *
 * ui  The UI system to register flows with.
 */
static void sol_register_buffer_save_command_defaults(SolUISystem *ui)
{
    if (!ui) return;
    const SolKeyCode save_sequence[] = { 'B', 'S' };
    (void)sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "buffer.save",
        .label  = "Save buffer",
        .sequence = save_sequence,
        .sequence_length = 2u,
    });
    const SolKeyCode save_all_sequence[] = { 'B', 'S' };
    const SolModifierMask save_all_mods[] = { SOL_MOD_NONE, SOL_MOD_SHIFT };
    (void)sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "buffer.save_all",
        .label  = "Save all buffers",
        .sequence = save_all_sequence,
        .step_modifiers = save_all_mods,
        .sequence_length = 2u,
    });
}

/* Register mouse-accessible title-bar entries for built-in workspace views. */
static void sol_register_workspace_menu_items(SolUISystem *ui)
{
    if (!ui) return;
    const SolUIMenuItemDesc items[] = {
        {
            .menu_id = "view", .menu_label = "View",
            .item_id = "explorer", .label = "Explorer",
            .action = "explorer.focus.toggle", .menu_order = 400, .item_order = 10,
        },
        {
            .menu_id = "view", .menu_label = "View",
            .item_id = "terminal", .label = "Terminal",
            .action = "terminal.toggle", .menu_order = 400, .item_order = 20,
        },
        {
            .menu_id = "edit", .menu_label = "Edit",
            .item_id = "undo", .label = "Undo",
            .action = "edit.undo", .menu_order = 300, .item_order = 10,
        },
        {
            .menu_id = "edit", .menu_label = "Edit",
            .item_id = "redo", .label = "Redo",
            .action = "edit.redo", .menu_order = 300, .item_order = 20,
        },
        {
            .menu_id = "edit", .menu_label = "Edit",
            .item_id = "find-files", .label = "Search Files...",
            .action = "find.files", .menu_order = 300, .item_order = 40,
        },
        {
            .menu_id = "edit", .menu_label = "Edit",
            .item_id = "find-content", .label = "Search in Files...",
            .action = "find.grep", .menu_order = 300, .item_order = 50,
        },
    };
    for (size_t i = 0u; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (sol_ui_system_register_menu_item(ui, &items[i]) ==
            SOL_UI_MENU_ITEM_TOKEN_INVALID) {
            fprintf(stderr, "sol: failed to register menu item '%s'\n",
                    items[i].item_id);
        }
    }
}

/* Open `path` into the active leaf, deduping against an existing
   buffer with the same source path. */
static bool sol_open_path_in_active_leaf(SolAppContext *app, const char *path)
{
    if (!app || !app->buffers || !path) return false;
    app->explorer_focused = false;
    app->focus_before_explorer = 0u;

    const SolBufferId existing = sol_text_buffer_find_by_path(app->buffers, path);
    if (existing != 0u) {
        return sol_buffer_set_active_leaf_buffer(app->buffers, existing);
    }

    const char *err = NULL;
    const SolBufferId id = sol_text_buffer_open_file(
        app->buffers, path, /* display name */ NULL,
        sol_text_view_render, &err);
    if (id == 0u) {
        fprintf(stderr, "sol: cannot open '%s': %s\n",
                path, err ? err : "unknown error");
        return false;
    }
    if (!sol_buffer_set_active_leaf_buffer(app->buffers, id)) {
        fprintf(stderr, "sol: failed to focus buffer for '%s'\n", path);
    }
    return true;
}

static bool sol_path_starts_with_dir(const char *path, const char *dir)
{
    if (!path || !dir || dir[0] == '\0') return false;
    const size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0) return false;
    return path[n] == '\0' || sol_platform_is_path_separator(path[n]) ||
           (n > 0u && sol_platform_is_path_separator(dir[n - 1u]));
}

static bool sol_parent_dir_of(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0u) return false;
    const char *last_sep = NULL;
    for (const char *p = path; *p != '\0'; ++p) {
        if (sol_platform_is_path_separator(*p)) last_sep = p;
    }
    if (!last_sep || last_sep == path) {
        if (out_size < 2u) return false;
        out[0] = last_sep ? path[0] : '.';
        out[1] = '\0';
        return true;
    }
    const size_t len = (size_t)(last_sep - path);
    if (len + 1u > out_size) return false;
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

static bool sol_context_target_dir(const SolUIContextActionRequest *request,
                                   char *out, size_t out_size)
{
    if (!request || !request->path || !out || out_size == 0u) return false;
    if (request->path_is_dir) {
        const size_t len = strlen(request->path);
        if (len + 1u > out_size) return false;
        memcpy(out, request->path, len + 1u);
        return true;
    }
    return sol_parent_dir_of(request->path, out, out_size);
}

static bool sol_make_unique_child_path(const char *dir,
                                       const char *stem,
                                       const char *extension,
                                       char *out,
                                       size_t out_size)
{
    if (!dir || !stem || !out || out_size == 0u) return false;
    const char *ext = extension ? extension : "";
    for (int i = 0; i < 10000; ++i) {
        char name[512];
        if (i == 0) {
            snprintf(name, sizeof(name), "%s%s", stem, ext);
        } else {
            snprintf(name, sizeof(name), "%s %d%s", stem, i + 1, ext);
        }
        char *joined = sol_platform_path_join(dir, name);
        if (!joined) return false;
        const size_t len = strlen(joined);
        bool fits = len + 1u <= out_size;
        SolPathInfo info;
        const bool exists = sol_platform_get_path_info(joined, &info);
        if (fits && !exists) {
            memcpy(out, joined, len + 1u);
            free(joined);
            return true;
        }
        free(joined);
    }
    return false;
}

static bool sol_make_unique_copy_path(const char *dir,
                                      const char *basename,
                                      char *out,
                                      size_t out_size)
{
    if (!dir || !basename || !out || out_size == 0u) return false;
    char stem[384];
    char ext[128];
    const char *dot = strrchr(basename, '.');
    if (dot && dot != basename) {
        const size_t stem_len = (size_t)(dot - basename);
        if (stem_len >= sizeof(stem) || strlen(dot) >= sizeof(ext)) return false;
        memcpy(stem, basename, stem_len);
        stem[stem_len] = '\0';
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        if (strlen(basename) >= sizeof(stem)) return false;
        snprintf(stem, sizeof(stem), "%s", basename);
        ext[0] = '\0';
    }

    char *joined = sol_platform_path_join(dir, basename);
    if (!joined) return false;
    const size_t direct_len = strlen(joined);
    SolPathInfo info;
    if (!sol_platform_get_path_info(joined, &info)) {
        if (direct_len + 1u > out_size) {
            free(joined);
            return false;
        }
        memcpy(out, joined, direct_len + 1u);
        free(joined);
        return true;
    }
    free(joined);

    char copy_stem[512];
    snprintf(copy_stem, sizeof(copy_stem), "%s copy", stem);
    return sol_make_unique_child_path(dir, copy_stem, ext, out, out_size);
}

/*
 * Re-scan the mounted explorer root after a filesystem change, without
 * signalling "the root path itself changed."
 *
 * Uses sol_ui_system_refresh_file_tree rather than re-setting the root
 * (which publishes SOL_EVENT_FILE_TREE_ROOT): every call site here is a
 * content change under the same root (file/folder create, paste,
 * delete, or an external change reported by the file watcher), never an
 * actual "open a different folder" — republishing the root-changed
 * event on each one caused subscribers that treat it as "discard and
 * rebuild everything" (e.g. the git plugin's full re-discovery) to
 * thrash on every single filesystem operation.
 */
static void sol_refresh_explorer(SolAppContext *app)
{
    if (!app || !app->ui) return;
    (void)sol_ui_system_refresh_file_tree(app->ui);
}

static bool sol_publish_command(SolAppContext *app, const char *action)
{
    if (!app || !app->events || !action) return false;
    SolCommandInvokedPayload payload;
    payload.action = action;
    sol_event_publish(app->events, SOL_EVENT_COMMAND_INVOKED,
                      &payload, sizeof(payload), app->ui);
    return true;
}

static void sol_focus_context_target(SolAppContext *app,
                                     const SolUIContextActionRequest *request)
{
    if (!app || !app->ui || !app->buffers || !request) return;
    if (request->leaf_id != 0u) {
        (void)sol_ui_system_focus_leaf(app->ui, request->leaf_id);
    }
    if (request->buffer_id != 0u) {
        (void)sol_buffer_set_active_leaf_buffer(app->buffers, request->buffer_id);
    }
}

static SolTextBuffer *sol_prepare_context_text_target(
    SolAppContext *app,
    const SolUIContextActionRequest *request,
    bool preserve_selection)
{
    if (!app || !request) return NULL;
    sol_focus_context_target(app, request);

    SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
    if (!tb) return NULL;

    if (!request->has_local_point) {
        return tb;
    }
    if (preserve_selection && sol_text_buffer_has_selection(tb)) {
        return tb;
    }

    size_t line = 0u;
    size_t cp_col = 0u;
    if (sol_text_view_local_point_to_line_col(app->ui, tb,
                                              request->local_x,
                                              request->local_y,
                                              &line, &cp_col)) {
        sol_text_buffer_set_cursor_to(tb, line, cp_col);
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
    return tb;
}

static void sol_close_buffers_under_path(SolAppContext *app, const char *path)
{
    if (!app || !app->buffers || !path) return;
    for (;;) {
        bool closed = false;
        const size_t count = sol_buffer_count(app->buffers);
        for (size_t i = 0u; i < count; ++i) {
            const SolBufferId id = sol_buffer_at(app->buffers, i);
            SolBuffer *buf = sol_buffer_get(app->buffers, id);
            SolTextBuffer *tb = sol_text_buffer_state(buf);
            const char *source = sol_text_buffer_source_path(tb);
            if (source && sol_path_starts_with_dir(source, path)) {
                (void)sol_buffer_close(app->buffers, id);
                closed = true;
                break;
            }
        }
        if (!closed) break;
    }
}

static void sol_show_error(SolAppContext *app, const char *message)
{
    if (!app || !app->instance || !message) return;
    (void)ca_popup_show(app->instance, &(Ca_PopupDesc){
        .title = "Sol",
        .message = message,
        .buttons = CA_POPUP_BUTTONS_OK,
        .replace_active = true,
    });
}

/*
 * Recompute the aggregated "external changes not loaded" status-bar
 * warning from scratch by scanning every open buffer's
 * external-change-pending flag, adding/updating/removing the segment as
 * needed. Called after every watcher drain and after every save, so the
 * warning always reflects exactly the set of buffers still unreconciled,
 * never stale and never left behind after the user saves over a conflict.
 *
 * app  The application context.
 */
static void sol_refresh_external_change_status(SolAppContext *app)
{
    if (!app || !app->buffers || !app->ui) return;

    char names[400] = "External changes not loaded (unsaved edits kept):";
    size_t len = strlen(names);
    bool any = false;

    const size_t count = sol_buffer_count(app->buffers);
    for (size_t i = 0u; i < count; ++i) {
        const SolBufferId id = sol_buffer_at(app->buffers, i);
        SolBuffer *buf = sol_buffer_get(app->buffers, id);
        SolTextBuffer *tb = sol_text_buffer_state(buf);
        if (!tb || !sol_text_buffer_has_external_change(tb)) continue;

        any = true;
        const char *name = sol_buffer_name(buf);
        int written = snprintf(names + len, sizeof(names) - len,
                               " %s;", name ? name : "?");
        if (written > 0) {
            const size_t w = (size_t)written;
            len += (w < sizeof(names) - len) ? w : sizeof(names) - len - 1u;
        }
    }

    if (any) {
        if (app->external_change_status == SOL_UI_STATUS_TOKEN_INVALID) {
            app->external_change_status = sol_ui_system_add_status_segment(
                app->ui, names, "status-bar-warning");
        } else {
            sol_ui_system_update_status_segment(
                app->ui, app->external_change_status, names);
        }
    } else if (app->external_change_status != SOL_UI_STATUS_TOKEN_INVALID) {
        sol_ui_system_remove_status_segment(app->ui, app->external_change_status);
        app->external_change_status = SOL_UI_STATUS_TOKEN_INVALID;
    }
}

/*
 * Save the active text buffer to its source path.
 *
 * Reports an error popup (e.g. no path yet, or a write failure) rather
 * than failing silently, since save is a user-initiated action whose
 * outcome the user needs to know. On success bumps the buffer system's
 * revision signal so the tab strip's dirty indicator redraws.
 *
 * app  The application context.
 * Returns  true if the active buffer was saved.
 */
static bool sol_save_active_buffer(SolAppContext *app)
{
    if (!app || !app->buffers) return false;
    SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
    if (!tb) return false;

    const char *err = NULL;
    if (!sol_text_buffer_save(tb, &err)) {
        char message[512];
        snprintf(message, sizeof(message), "Could not save file: %s",
                 err ? err : "unknown error");
        sol_show_error(app, message);
        return false;
    }
    sol_buffer_touch(app->buffers, sol_buffer_active_buffer(app->buffers));
    sol_refresh_external_change_status(app);
    return true;
}

/*
 * Save every dirty text buffer to disk.
 *
 * Does not stop at the first failure — every dirty buffer that can be
 * saved is saved, and any failures are reported together in a single
 * popup, so one locked or unwritable file never blocks the rest.
 *
 * app  The application context.
 * Returns  true if at least one buffer was successfully saved (false
 *          when there was nothing dirty to save, or every save failed).
 */
static bool sol_save_all_dirty_buffers(SolAppContext *app)
{
    if (!app || !app->buffers) return false;

    size_t saved = 0u;
    size_t failed = 0u;
    char failure_summary[512] = "Could not save:";
    size_t failure_len = strlen(failure_summary);

    const size_t count = sol_buffer_count(app->buffers);
    for (size_t i = 0u; i < count; ++i) {
        const SolBufferId id = sol_buffer_at(app->buffers, i);
        SolBuffer *buf = sol_buffer_get(app->buffers, id);
        SolTextBuffer *tb = sol_text_buffer_state(buf);
        if (!tb || !sol_text_buffer_is_dirty(tb)) continue;

        const char *err = NULL;
        if (sol_text_buffer_save(tb, &err)) {
            sol_buffer_touch(app->buffers, id);
            saved++;
        } else {
            failed++;
            const char *name = sol_buffer_name(buf);
            int n = snprintf(failure_summary + failure_len,
                             sizeof(failure_summary) - failure_len,
                             " %s (%s);", name ? name : "?",
                             err ? err : "unknown error");
            if (n > 0) {
                const size_t written = (size_t)n;
                failure_len += (written < sizeof(failure_summary) - failure_len)
                    ? written : sizeof(failure_summary) - failure_len - 1u;
            }
        }
    }

    if (failed > 0u) {
        sol_show_error(app, failure_summary);
    }
    if (saved > 0u) {
        sol_refresh_external_change_status(app);
    }
    return saved > 0u;
}

static bool sol_context_create_file(SolAppContext *app,
                                    const SolUIContextActionRequest *request)
{
    char dir[4096];
    char path[4096];
    if (!sol_context_target_dir(request, dir, sizeof(dir)) ||
        !sol_make_unique_child_path(dir, "untitled", ".txt", path, sizeof(path))) {
        sol_show_error(app, "Could not choose a path for the new file.");
        return false;
    }
    if (!sol_platform_create_empty_file(path, true)) {
        sol_show_error(app, "Could not create the file.");
        return false;
    }
    sol_refresh_explorer(app);
    return sol_open_path_in_active_leaf(app, path);
}

static bool sol_context_create_folder(SolAppContext *app,
                                      const SolUIContextActionRequest *request)
{
    char dir[4096];
    char path[4096];
    if (!sol_context_target_dir(request, dir, sizeof(dir)) ||
        !sol_make_unique_child_path(dir, "New Folder", "", path, sizeof(path))) {
        sol_show_error(app, "Could not choose a path for the new folder.");
        return false;
    }
    if (!sol_platform_mkdir_p(path)) {
        sol_show_error(app, "Could not create the folder.");
        return false;
    }
    sol_refresh_explorer(app);
    return true;
}

static bool sol_context_copy_or_cut_path(SolAppContext *app,
                                         const SolUIContextActionRequest *request,
                                         bool cut)
{
    if (!app || !request || !request->path) return false;
    const size_t len = strlen(request->path);
    if (len >= sizeof(app->file_clipboard_path)) return false;
    memcpy(app->file_clipboard_path, request->path, len + 1u);
    app->file_clipboard_cut = cut;
    return true;
}

static bool sol_context_paste_path(SolAppContext *app,
                                   const SolUIContextActionRequest *request)
{
    if (!app || !request || app->file_clipboard_path[0] == '\0') {
        return false;
    }

    char dir[4096];
    if (!sol_context_target_dir(request, dir, sizeof(dir))) {
        sol_show_error(app, "Could not choose a paste destination.");
        return false;
    }

    const char *base = sol_platform_basename(app->file_clipboard_path);
    char dest[4096];
    if (!sol_make_unique_copy_path(dir, base, dest, sizeof(dest))) {
        sol_show_error(app, "Could not choose a paste path.");
        return false;
    }

    if (sol_path_starts_with_dir(dest, app->file_clipboard_path)) {
        sol_show_error(app, "Cannot paste a folder inside itself.");
        return false;
    }

    bool ok = app->file_clipboard_cut
        ? sol_platform_move_path(app->file_clipboard_path, dest)
        : sol_platform_copy_path_recursive(app->file_clipboard_path, dest);
    if (!ok) {
        sol_show_error(app, "Paste failed.");
        return false;
    }

    if (app->file_clipboard_cut) {
        app->file_clipboard_path[0] = '\0';
        app->file_clipboard_cut = false;
    }
    sol_refresh_explorer(app);
    return true;
}

static void sol_confirm_delete_result(Ca_PopupResult result, void *user_data)
{
    SolDeletePathRequest *pending = (SolDeletePathRequest *)user_data;
    if (!pending) return;
    SolAppContext *app = pending->app;
    if (result == CA_POPUP_RESULT_YES && app) {
        sol_close_buffers_under_path(app, pending->path);
        if (!sol_platform_remove_path_recursive(pending->path)) {
            sol_show_error(app, "Delete failed.");
        } else {
            sol_refresh_explorer(app);
        }
    }
    free(pending);
}

static bool sol_context_delete_path(SolAppContext *app,
                                    const SolUIContextActionRequest *request)
{
    if (!app || !app->instance || !request || !request->path) return false;
    SolDeletePathRequest *pending =
        (SolDeletePathRequest *)calloc(1u, sizeof(SolDeletePathRequest));
    if (!pending) return false;
    pending->app = app;
    const size_t len = strlen(request->path);
    if (len >= sizeof(pending->path)) {
        free(pending);
        return false;
    }
    memcpy(pending->path, request->path, len + 1u);

    char message[4608];
    snprintf(message, sizeof(message), "Delete \"%s\"?", request->path);
    if (!ca_popup_show(app->instance, &(Ca_PopupDesc){
            .title = "Delete",
            .message = message,
            .buttons = CA_POPUP_BUTTONS_YES_NO,
            .replace_active = false,
            .queue_if_busy = true,
            .on_result = sol_confirm_delete_result,
            .result_data = pending,
        })) {
        free(pending);
        return false;
    }
    return true;
}

static bool sol_on_context_action(const SolUIContextActionRequest *request,
                                  void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !request) return false;

    switch (request->action) {
    case SOL_UI_CONTEXT_ACTION_OPEN:
        if (!request->path) return false;
        if (request->path_is_dir) {
            return sol_set_explorer_root(app, request->path);
        }
        return sol_open_path_in_active_leaf(app, request->path);
    case SOL_UI_CONTEXT_ACTION_OPEN_FILE_PICKER:
        return sol_publish_command(app, "buffer.open");
    case SOL_UI_CONTEXT_ACTION_OPEN_FOLDER_PICKER:
        return sol_publish_command(app, "explorer.open");
    case SOL_UI_CONTEXT_ACTION_NEW_BUFFER:
        return sol_publish_command(app, "buffer.new");
    case SOL_UI_CONTEXT_ACTION_CLOSE_BUFFER:
        sol_focus_context_target(app, request);
        return sol_publish_command(app, "buffer.close");
    case SOL_UI_CONTEXT_ACTION_CLOSE_TAB:
        return sol_buffer_close_leaf_tab(app->buffers, request->leaf_id,
                                         request->buffer_id);
    case SOL_UI_CONTEXT_ACTION_CLOSE_ALL_BUFFERS:
        return sol_buffer_close_all(app->buffers) > 0u;
    case SOL_UI_CONTEXT_ACTION_SPLIT_VERTICAL:
        sol_focus_context_target(app, request);
        return sol_publish_command(app, "pane.split.vertical");
    case SOL_UI_CONTEXT_ACTION_SPLIT_HORIZONTAL:
        sol_focus_context_target(app, request);
        return sol_publish_command(app, "pane.split.horizontal");
    case SOL_UI_CONTEXT_ACTION_NEW_FILE:
        return sol_context_create_file(app, request);
    case SOL_UI_CONTEXT_ACTION_NEW_FOLDER:
        return sol_context_create_folder(app, request);
    case SOL_UI_CONTEXT_ACTION_DELETE_PATH:
        return sol_context_delete_path(app, request);
    case SOL_UI_CONTEXT_ACTION_COPY_PATH:
        return sol_context_copy_or_cut_path(app, request, false);
    case SOL_UI_CONTEXT_ACTION_CUT_PATH:
        return sol_context_copy_or_cut_path(app, request, true);
    case SOL_UI_CONTEXT_ACTION_PASTE_PATH:
        return sol_context_paste_path(app, request);
    case SOL_UI_CONTEXT_ACTION_COPY_TEXT: {
        SolTextBuffer *tb = sol_prepare_context_text_target(app, request, true);
        if (!tb) return false;
        return sol_publish_command(app, sol_text_buffer_has_selection(tb)
                                          ? "edit.copy"
                                          : "edit.copy_line");
    }
    case SOL_UI_CONTEXT_ACTION_COPY_LINE:
        if (!sol_prepare_context_text_target(app, request, false)) return false;
        return sol_publish_command(app, "edit.copy_line");
    case SOL_UI_CONTEXT_ACTION_CUT_TEXT: {
        SolTextBuffer *tb = sol_prepare_context_text_target(app, request, true);
        if (!tb) return false;
        if (sol_text_buffer_has_selection(tb)) {
            return sol_publish_command(app, "edit.cut");
        }
        if (!sol_publish_command(app, "edit.copy_line")) return false;
        return sol_publish_command(app, "edit.delete_line");
    }
    case SOL_UI_CONTEXT_ACTION_PASTE_TEXT:
        if (!sol_prepare_context_text_target(app, request, true)) return false;
        return sol_publish_command(app, "edit.paste");
    case SOL_UI_CONTEXT_ACTION_PASTE_LINE:
        if (!sol_prepare_context_text_target(app, request, false)) return false;
        return sol_publish_command(app, "edit.paste_line");
    case SOL_UI_CONTEXT_ACTION_SELECT_ALL_TEXT:
        sol_focus_context_target(app, request);
        return sol_publish_command(app, "edit.select_all");
    case SOL_UI_CONTEXT_ACTION_DELETE_TEXT:
        if (!sol_prepare_context_text_target(app, request, true)) return false;
        return sol_publish_command(app, "edit.delete_char");
    case SOL_UI_CONTEXT_ACTION_DELETE_LINE:
        if (!sol_prepare_context_text_target(app, request, false)) return false;
        return sol_publish_command(app, "edit.delete_line");
    case SOL_UI_CONTEXT_ACTION_NONE:
    default:
        return false;
    }
}

/* File-tree click → open. */
static bool sol_on_tree_file_open(const char *path, void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    return sol_open_path_in_active_leaf(app, path);
}

/* File-picker callbacks. */
static void sol_on_picker_file_chosen(const char *path, void *user_data)
{
    if (!path) return;   /* cancelled */
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app) return;
    if (sol_open_path_in_active_leaf(app, path) && app->ui) {
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_picker_folder_chosen(const char *path, void *user_data)
{
    if (!path) return;
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->ui) return;
    if (!sol_set_explorer_root(app, path)) {
        fprintf(stderr, "sol: cannot open directory '%s'\n", path);
    }
}

static void sol_on_menu_new_buffer(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->buffers) return;
    const SolBufferId id = sol_text_buffer_open_empty(
        app->buffers, "untitled", sol_text_view_render);
    if (id == 0u) return;
    sol_buffer_set_active_leaf_buffer(app->buffers, id);
}

static void sol_on_menu_open_file(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance, SOL_FILE_PICKER_FILE, NULL,
                         sol_on_picker_file_chosen, app);
}

static void sol_on_menu_open_folder(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance, SOL_FILE_PICKER_FOLDER, NULL,
                         sol_on_picker_folder_chosen, app);
}

/* UI focus bridge: keep explorer-focus state synced to actual widget
    interactions emitted by the UI system (not geometry guesses). */
static void sol_on_ui_focus_region(bool in_tree_panel, void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->ui || !app->buffers) return;

    if (sol_ui_system_tree_panel_width(app->ui) <= 0) {
        app->explorer_focused = false;
        app->focus_before_explorer = 0u;
        return;
    }

    if (in_tree_panel) {
        if (!app->explorer_focused) {
            SolBufferNodeId leaf = sol_buffer_active_leaf(app->buffers);
            if (leaf != 0u) {
                app->focus_before_explorer = leaf;
            }
        }
        app->explorer_focused = true;
        return;
    }

    app->explorer_focused = false;
    app->focus_before_explorer = 0u;
}

/* Snapshot whichever buffer leaf was active just before the terminal claims
   keyboard focus.  Fired by sol_ui_system_terminal_set_focused(true) — both
   the command path and the click-on-panel path go through that helper. */
static void sol_on_terminal_focus_gain(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->buffers) return;
    const SolBufferNodeId leaf = sol_buffer_active_leaf(app->buffers);
    if (leaf != 0u) {
        app->focus_before_terminal = leaf;
    }
}

/*
 * Forward an OSC 52 clipboard write from a terminal application to the
 * OS clipboard. Wired via sol_terminal_manager_set_clipboard_write so tools
 * running inside Sol's terminal (tmux, Neovim, Claude Code, etc.) can copy
 * a selection out to the host clipboard the same way a native app would.
 */
static void sol_on_terminal_clipboard_write(const char *text, void *user_data)
{
    Ca_Window *window = (Ca_Window *)user_data;
    if (!window || !text || text[0] == '\0') return;
    ca_clipboard_set_text(window, text);
}

/* ------------------------------------------------------------------ */
/* Startup event + warmup                                              */
/* ------------------------------------------------------------------ */

static bool sol_on_startup_event(const SolEvent *event, void *user_data)
{
    (void)user_data;
    if (!event || !event->payload ||
        event->payload_size != sizeof(SolAppStartupPayload)) {
        return false;
    }
    const SolAppStartupPayload *p = (const SolAppStartupPayload *)event->payload;
    printf("[sol] startup: workers=%u plugins=%u input=%s\n",
           p->worker_count, p->loaded_plugins,
           p->input_binding_active ? "ready" : "missing");
    return false;
}

/* ------------------------------------------------------------------ */
/* Built-in command actions (event-driven)                             */
/* ------------------------------------------------------------------ */

/* Find the next/prev buffer in registration order, excluding `current`.
   Returns 0u when there are fewer than 2 live buffers (caller should
   treat that as "leave the new pane empty"). */
static SolBufferId sol_next_buffer_in_cycle(SolBufferSystem *buffers,
                                            SolBufferId current,
                                            int direction)
{
    if (!buffers) return 0u;
    const size_t total = sol_buffer_count(buffers);
    if (total < 2u) return 0u;

    size_t cur_idx = total;   /* sentinel: not found */
    for (size_t i = 0u; i < total; ++i) {
        if (sol_buffer_at(buffers, i) == current) {
            cur_idx = i;
            break;
        }
    }
    size_t next_idx;
    if (cur_idx == total) {
        next_idx = 0u;
    } else if (direction > 0) {
        next_idx = (cur_idx + 1u) % total;
    } else {
        next_idx = (cur_idx + total - 1u) % total;
    }
    return sol_buffer_at(buffers, next_idx);
}

static bool sol_focus_buffer_by_index(SolBufferSystem *buffers, size_t index)
{
    if (!buffers) return false;
    const SolBufferId id = sol_buffer_at(buffers, index);
    if (id == 0u) return false;
    return sol_buffer_set_active_leaf_buffer(buffers, id);
}

/*
 * Subscriber for SOL_EVENT_TEXT_EDITED that drives autosave debouncing.
 *
 * Pushes the shared autosave deadline out to (this edit's timestamp +
 * debounce) whenever autosave is enabled, so the frame loop's sweep
 * (see sol_run_autosave_sweep) only fires once edits settle rather than
 * on every keystroke. A no-op while autosave is off, so toggling it off
 * cleanly stops any pending sweep from ever triggering.
 *
 * event      The SOL_EVENT_TEXT_EDITED event.
 * user_data  The SolAppContext.
 * Returns    false always — this handler never marks the event handled,
 *            so other subscribers (syntax highlighting, etc.) still run.
 */
static bool sol_on_text_edited_for_autosave(const SolEvent *event, void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->settings.autosave_enabled) return false;
    app->autosave_deadline_ns = event->timestamp_ns + SOL_AUTOSAVE_DEBOUNCE_NS;
    /* The main loop blocks in glfwWaitEvents() between input events, so
       without this the debounce deadline would only ever be checked the
       next time the user happens to move the mouse or press a key —
       autosave would silently never fire while idle. This wakes the
       loop once the debounce window elapses even with no further input. */
    if (app->instance) {
        ca_instance_request_frame_after(
            app->instance, (double)SOL_AUTOSAVE_DEBOUNCE_NS / 1e9);
    }
    return false;
}

/*
 * Save every dirty buffer once the autosave debounce deadline has passed.
 *
 * Called once per frame from the main loop. Cheap no-op in the common
 * case (deadline unset, or not yet reached). Reuses
 * sol_save_all_dirty_buffers so autosave shares the exact same
 * crash-safe atomic-write path as manual Save/Save All — it is a
 * scheduled caller of that path, not a separate save implementation.
 *
 * app  The application context.
 */
static void sol_run_autosave_sweep(SolAppContext *app)
{
    if (!app || app->autosave_deadline_ns == 0u) return;
    if (!app->settings.autosave_enabled) {
        app->autosave_deadline_ns = 0u;
        return;
    }
    if (sol_platform_now_monotonic_ns() < app->autosave_deadline_ns) return;

    app->autosave_deadline_ns = 0u;
    (void)sol_save_all_dirty_buffers(app);
}

/*
 * React to one external filesystem change for a path that matches an
 * open text buffer's source_path.
 *
 * Conflict policy (mirrors VS Code): a clean buffer is silently reloaded
 * from disk — the user has nothing to lose and the on-disk version is
 * simply newer. A dirty buffer is left completely untouched; its
 * content is never overwritten by an external change the user didn't
 * initiate. The caller (sol_drain_file_watcher) surfaces a status-bar
 * warning for any buffer this function leaves dirty-and-stale so the
 * user knows to reconcile manually (re-save or close-and-reopen).
 *
 * app  The application context.
 * id   Id of the buffer whose source_path matched the changed path.
 * Returns  true if the buffer was reloaded (i.e. it was clean).
 */
static bool sol_handle_external_change_for_buffer(SolAppContext *app, SolBufferId id)
{
    SolBuffer *buf = sol_buffer_get(app->buffers, id);
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return false;
    if (sol_text_buffer_is_dirty(tb)) return false;

    const char *err = NULL;
    if (!sol_text_buffer_reload_from_disk(tb, &err)) {
        /* Read failure (e.g. file briefly missing mid-write elsewhere) —
           leave the in-memory content exactly as it was; nothing to warn
           about since no edits are at risk. */
        return false;
    }
    sol_buffer_touch(app->buffers, id);
    sol_ui_system_invalidate_buffer_area(app->ui);
    return true;
}

/*
 * Drain the file watcher's queued events once per frame and react.
 *
 * Always refreshes the explorer tree (once per drained batch, not per
 * event) when any event falls under the watched root. For events under
 * an open buffer's path: reloads clean buffers, and tracks dirty ones
 * that were left stale so a single aggregated status-bar warning can be
 * shown/hidden — never a blocking popup, since an external change is
 * not something the user needs to act on immediately.
 *
 * app  The application context.
 */
static void sol_drain_file_watcher(SolAppContext *app)
{
    if (!app || !app->watcher || !app->buffers) return;

    SolFileWatchEvent events[32];
    const size_t n = sol_file_watcher_poll(app->watcher, events, 32u);
    if (n == 0u) return;

    const char *root = sol_ui_system_file_tree_root(app->ui);
    bool any_under_root = false;
    for (size_t i = 0u; i < n; ++i) {
        if (sol_path_starts_with_dir(events[i].path, root ? root : "")) {
            any_under_root = true;
            break;
        }
    }
    if (any_under_root) {
        sol_refresh_explorer(app);
    }

    bool any_new_conflict = false;
    for (size_t i = 0u; i < n; ++i) {
        const SolBufferId id = sol_text_buffer_find_by_path(app->buffers, events[i].path);
        if (id == 0u) continue;   /* no open buffer for this path */

        if (!sol_handle_external_change_for_buffer(app, id)) {
            SolBuffer *buf = sol_buffer_get(app->buffers, id);
            SolTextBuffer *tb = sol_text_buffer_state(buf);
            if (tb && sol_text_buffer_is_dirty(tb)) {
                sol_text_buffer_set_external_change_pending(tb);
                any_new_conflict = true;
            }
        }
    }

    if (any_new_conflict) {
        sol_refresh_external_change_status(app);
    }
}

static bool sol_toggle_explorer_focus(SolAppContext *app)
{
    if (!app || !app->ui || !app->buffers) return false;

    if (app->explorer_focused && sol_ui_system_file_tree_active(app->ui)) {
        app->explorer_focused = false;
        sol_ui_system_set_file_tree_visible(app->ui, false);
        SolBufferNodeId restore = app->focus_before_explorer;
        if (restore == 0u) {
            /* First-time explorer toggle with no prior anchor: ensure
               focus lands back in the active buffer pane. */
            restore = sol_buffer_active_leaf(app->buffers);
        }
        if (restore != 0u) {
            (void)sol_ui_system_focus_leaf(app->ui, restore);
        }
        app->focus_before_explorer = 0u;
        return true;
    }

    const char *root = sol_ui_system_file_tree_root(app->ui);
    char cwd_buf[4096];
    if (!root || root[0] == '\0') {
        if (!sol_platform_get_cwd(cwd_buf, sizeof(cwd_buf))) {
            return false;
        }
        if (!sol_set_explorer_root(app, cwd_buf)) {
            return false;
        }
    }
    sol_ui_system_show_file_tree(app->ui);

    /* Capture exactly once per explorer-focus session: the last thing
       focused before entering explorer. */
    app->focus_before_explorer = sol_buffer_active_leaf(app->buffers);
    app->explorer_focused = true;
    sol_ui_system_set_focused_panel(app->ui, SOL_UI_FOCUSED_PANEL_TREE);
    return true;
}

/* Action dispatcher subscribed to SOL_EVENT_COMMAND_INVOKED. The action
   string carries the verb; user_data is the SolAppContext so handlers
   can reach the buffer system. New actions can be added here OR by
   plugins that subscribe to the same event with their own filtering. */
static bool sol_on_command_invoked(const SolEvent *event, void *user_data)
{
    if (!event || !event->payload ||
        event->payload_size < sizeof(SolCommandInvokedPayload)) {
        return false;
    }
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->buffers) return false;

    const SolCommandInvokedPayload *p =
        (const SolCommandInvokedPayload *)event->payload;
    if (!p->action) return false;

    if (strcmp(p->action, "explorer.focus.toggle") == 0) {
        return sol_toggle_explorer_focus(app);
    }

    /* All non-explorer actions implicitly dismiss explorer focus state. */
    app->explorer_focused = false;
    app->focus_before_explorer = 0u;

    /* ---- buffer.new : open a fresh empty buffer in the active leaf. */
    if (strcmp(p->action, "buffer.new") == 0) {
        const SolBufferId id = sol_text_buffer_open_empty(
            app->buffers, "untitled", sol_text_view_render);
        if (id == 0u) return false;
        return sol_buffer_set_active_leaf_buffer(app->buffers, id);
    }

    /* ---- buffer.open : open a file from disk via the file picker. */
    if (strcmp(p->action, "buffer.open") == 0) {
        sol_file_picker_open(app->instance, SOL_FILE_PICKER_FILE, NULL,
                             sol_on_picker_file_chosen, app);
        return true;
    }

    /* ---- buffer.close : close the active buffer. */
    if (strcmp(p->action, "buffer.close") == 0) {
        const SolBufferId id = sol_buffer_active_buffer(app->buffers);
        if (id == 0u) return false;
        return sol_buffer_close(app->buffers, id);
    }

    /* ---- buffer.save : write the active text buffer to its source path. */
    if (strcmp(p->action, "buffer.save") == 0) {
        return sol_save_active_buffer(app);
    }

    /* ---- buffer.save_all : write every dirty text buffer to disk.
       Saves everything that can be saved rather than stopping at the
       first failure, so one locked/unwritable file doesn't block the
       rest from being saved. */
    if (strcmp(p->action, "buffer.save_all") == 0) {
        return sol_save_all_dirty_buffers(app);
    }

    /* ---- buffer.focus.* */
    if (strcmp(p->action, "buffer.focus.previous") == 0) {
        /* When keyboard focus is elsewhere (terminal, file tree), the
           user's intent is "take me back to the buffer" — just restore
           focus to what is already showing. Only cycle to the
           previously-used buffer when the buffer panel already has
           focus, matching the alternate-buffer semantics this command
           is named for. Otherwise a stray tab-swap would fire on the
           same keystroke that was meant purely to return focus. */
        if (sol_ui_system_focused_panel(app->ui) != SOL_UI_FOCUSED_PANEL_BUFFER) {
            sol_ui_system_set_focused_panel(app->ui, SOL_UI_FOCUSED_PANEL_BUFFER);
            sol_input_router_set_buffer_input_active(app->router, true);
            return true;
        }
        return sol_buffer_focus_previous_buffer(app->buffers);
    }
    if (strcmp(p->action, "buffer.focus.first") == 0) {
        return sol_focus_buffer_by_index(app->buffers, 0u);
    }
    if (strcmp(p->action, "buffer.focus.last") == 0) {
        const size_t total = sol_buffer_count(app->buffers);
        return (total > 0u) ? sol_focus_buffer_by_index(app->buffers, total - 1u)
                            : false;
    }

    /* ---- buffer.cycle.* */
    if (strcmp(p->action, "buffer.cycle.next") == 0) {
        return sol_buffer_cycle_active_leaf(app->buffers, +1);
    }
    if (strcmp(p->action, "buffer.cycle.prev") == 0) {
        return sol_buffer_cycle_active_leaf(app->buffers, -1);
    }

    /* ---- pane.split.* : new pane shows the next buffer in cycle when
       >=2 buffers exist; otherwise an empty leaf. */
    if (strcmp(p->action, "pane.split.vertical") == 0 ||
        strcmp(p->action, "pane.split.horizontal") == 0)
    {
        const SolBufferSplitDirection dir =
            (strstr(p->action, "vertical") != NULL)
                ? SOL_BUFFER_SPLIT_VERTICAL
                : SOL_BUFFER_SPLIT_HORIZONTAL;
        const SolBufferId current = sol_buffer_active_buffer(app->buffers);
        const SolBufferId target  =
            sol_next_buffer_in_cycle(app->buffers, current, +1);
        return sol_buffer_split_active(app->buffers, dir, 0.5f, target, NULL);
    }

    /* ---- pane.focus.* */
    if (strcmp(p->action, "pane.focus.next") == 0) {
        return sol_buffer_cycle_active_pane(app->buffers, +1);
    }
    if (strcmp(p->action, "pane.focus.prev") == 0) {
        return sol_buffer_cycle_active_pane(app->buffers, -1);
    }

    /* ---- explorer.open : open a folder in the explorer panel via picker. */
    if (strcmp(p->action, "explorer.open") == 0) {
        sol_file_picker_open(app->instance, SOL_FILE_PICKER_FOLDER, NULL,
                             sol_on_picker_folder_chosen, app);
        return true;
    }

    if (strcmp(p->action, "find.files") == 0) {
        sol_ui_system_open_file_search(app->ui);
        return true;
    }
    if (strcmp(p->action, "find.grep") == 0) {
        sol_ui_system_open_content_search(app->ui);
        return true;
    }

    /* ---- edit.copy : copy selection to clipboard. */
    if (strcmp(p->action, "edit.copy") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb || !sol_text_buffer_has_selection(tb)) return false;
        char buf[65536];
        const size_t n = sol_text_buffer_copy_selection_bytes(tb, buf, sizeof(buf) - 1u);
        if (n == 0u) return false;
        buf[n] = '\0';
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        ca_clipboard_set_text(win, buf);
        return true;
    }

    /* ---- edit.copy_word : copy word at cursor (or selection) to clipboard. */
    if (strcmp(p->action, "edit.copy_word") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        char buf[65536];
        size_t n;
        if (sol_text_buffer_has_selection(tb)) {
            n = sol_text_buffer_copy_selection_bytes(tb, buf, sizeof(buf) - 1u);
        } else {
            const size_t saved = sol_text_buffer_cursor_byte(tb);
            sol_text_buffer_move_word(tb, -1, false);
            sol_text_buffer_move_word(tb, +1, true);
            n = sol_text_buffer_copy_selection_bytes(tb, buf, sizeof(buf) - 1u);
            sol_text_buffer_clear_selection(tb);
            sol_text_buffer_set_cursor_byte(tb, saved);
        }
        if (n == 0u) return false;
        buf[n] = '\0';
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        ca_clipboard_set_text(win, buf);
        return true;
    }

    /* ---- edit.copy_line : copy the current line (without trailing newline). */
    if (strcmp(p->action, "edit.copy_line") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        char buf[65536];
        const size_t line = sol_text_buffer_cursor_line(tb);
        const size_t n = sol_text_buffer_copy_line(tb, line, buf, sizeof(buf) - 1u);
        buf[n] = '\0';
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        ca_clipboard_set_text(win, buf);
        return true;
    }

    /* ---- edit.paste : insert clipboard text at cursor (replacing selection). */
    if (strcmp(p->action, "edit.paste") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        const char *text = ca_clipboard_get_text(win);
        if (!text || text[0] == '\0') return false;
        if (sol_text_buffer_has_selection(tb))
            sol_text_buffer_delete_selection(tb);
        const size_t at = sol_text_buffer_cursor_byte(tb);
        const size_t len = strlen(text);
        if (!sol_text_buffer_insert_bytes(tb, at, text, len)) return false;
        sol_text_buffer_set_cursor_byte(tb, at + len);
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.paste_line : insert clipboard text as a new line below cursor. */
    if (strcmp(p->action, "edit.paste_line") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        const char *text = ca_clipboard_get_text(win);
        if (!text || text[0] == '\0') return false;
        /* Find the end of the current line (byte offset of the '\n' or EOF). */
        const SolRope *rope = sol_text_buffer_rope(
            sol_buffer_get(app->buffers, sol_buffer_active_buffer(app->buffers)));
        if (!rope) return false;
        const size_t line      = sol_text_buffer_cursor_line(tb);
        const size_t line_b    = sol_rope_byte_of_line(rope, line);
        const size_t line_len  = sol_text_buffer_line_len(tb, line);
        const size_t insert_at = line_b + line_len; /* before the '\n' */
        /* Build "\n<text>" — strip any leading/trailing newlines from text. */
        const size_t tlen = strlen(text);
        char *ins = (char *)malloc(tlen + 2u);
        if (!ins) return false;
        ins[0] = '\n';
        memcpy(ins + 1u, text, tlen);
        /* Strip a single trailing newline if present so we don't add a blank. */
        size_t ins_len = tlen + 1u;
        if (ins_len > 1u && ins[ins_len - 1u] == '\n') ins_len--;
        if (!sol_text_buffer_insert_bytes(tb, insert_at, ins, ins_len)) {
            free(ins); return false;
        }
        free(ins);
        sol_text_buffer_set_cursor_byte(tb, insert_at + 1u);
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.undo --------------------------------------------------------- */
    if (strcmp(p->action, "edit.undo") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb || !sol_text_buffer_can_undo(tb)) return false;
        sol_text_buffer_undo(tb);
        sol_text_buffer_ensure_cursor_visible(tb,
            sol_active_buffer_viewport_lines(app, tb));
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.redo --------------------------------------------------------- */
    if (strcmp(p->action, "edit.redo") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb || !sol_text_buffer_can_redo(tb)) return false;
        sol_text_buffer_redo(tb);
        sol_text_buffer_ensure_cursor_visible(tb,
            sol_active_buffer_viewport_lines(app, tb));
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.select_all --------------------------------------------------- */
    if (strcmp(p->action, "edit.select_all") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        sol_text_buffer_select_all(tb);
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.delete_char : delete selection if active, else one char forward. */
    if (strcmp(p->action, "edit.delete_char") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        const bool changed = sol_text_buffer_has_selection(tb)
            ? sol_text_buffer_delete_selection(tb)
            : sol_text_buffer_delete_forward(tb);
        if (changed) sol_ui_system_invalidate_buffer_area(app->ui);
        return changed;
    }

    /* ---- edit.delete_word : delete one word forward (or selection). -------- */
    if (strcmp(p->action, "edit.delete_word") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        const bool changed = sol_text_buffer_delete_word_forward(tb);
        if (changed) sol_ui_system_invalidate_buffer_area(app->ui);
        return changed;
    }

    /* ---- edit.delete_word_back : delete one word backward. ----------------- */
    if (strcmp(p->action, "edit.delete_word_back") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        const bool changed = sol_text_buffer_delete_word_back(tb);
        if (changed) sol_ui_system_invalidate_buffer_area(app->ui);
        return changed;
    }

    /* ---- edit.delete_line : delete the entire current line. ---------------- */
    if (strcmp(p->action, "edit.delete_line") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        const bool changed = sol_text_buffer_delete_line(tb);
        if (changed) sol_ui_system_invalidate_buffer_area(app->ui);
        return changed;
    }

    /* ---- edit.cut : copy selection to clipboard then delete it. ------------ */
    if (strcmp(p->action, "edit.cut") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb || !sol_text_buffer_has_selection(tb)) return false;
        char buf[65536];
        const size_t n = sol_text_buffer_copy_selection_bytes(
            tb, buf, sizeof(buf) - 1u);
        if (n == 0u) return false;
        buf[n] = '\0';
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        ca_clipboard_set_text(win, buf);
        sol_text_buffer_delete_selection(tb);
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- terminal.* actions ---- */
    if (strncmp(p->action, "terminal.", 9) == 0) {
        SolTerminalManager *mgr = app->terminal_mgr;
        if (!mgr) return false;

        if (strcmp(p->action, "terminal.toggle") == 0) {
            if (sol_terminal_manager_count(mgr) == 0u) {
                const char *root = sol_ui_system_file_tree_root(app->ui);
                if (!sol_terminal_manager_new_tab(mgr, root)) return false;
            }
            const bool currently_focused = sol_terminal_manager_focused(mgr);
            const bool currently_visible = sol_terminal_manager_visible(mgr);
            if (!currently_visible) {
                sol_terminal_manager_set_visible(mgr, true);
                sol_ui_system_terminal_set_focused(app->ui, true);
            } else if (!currently_focused) {
                sol_ui_system_terminal_set_focused(app->ui, true);
            } else {
                /* Already visible and focused: hide and restore prior focus. */
                sol_terminal_manager_set_focused(mgr, false);
                sol_terminal_manager_set_visible(mgr, false);
                const SolBufferNodeId restore = app->focus_before_terminal;
                if (restore != 0u) {
                    (void)sol_ui_system_focus_leaf(app->ui, restore);
                    app->focus_before_terminal = 0u;
                }
            }
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.position.bottom") == 0) {
            sol_terminal_manager_set_position(mgr, SOL_TERMINAL_POSITION_BOTTOM);
            if (sol_terminal_manager_visible(mgr)) {
                sol_ui_system_terminal_set_focused(app->ui, true);
            }
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.position.right") == 0) {
            sol_terminal_manager_set_position(mgr, SOL_TERMINAL_POSITION_RIGHT);
            if (sol_terminal_manager_visible(mgr)) {
                sol_ui_system_terminal_set_focused(app->ui, true);
            }
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.kill") == 0) {
            if (sol_terminal_manager_count(mgr) == 0u) return false;
            sol_terminal_kill(sol_terminal_manager_active(mgr));
            sol_terminal_manager_close_active(mgr);
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.tab.new") == 0) {
            const char *root = sol_ui_system_file_tree_root(app->ui);
            SolTerminal *t = sol_terminal_manager_new_tab(mgr, root);
            if (!t) return false;
            sol_terminal_manager_set_visible(mgr, true);
            sol_ui_system_terminal_set_focused(app->ui, true);
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.tab.next") == 0) {
            if (sol_terminal_manager_count(mgr) == 0u) return false;
            sol_terminal_manager_next_tab(mgr);
            sol_terminal_manager_set_visible(mgr, true);
            sol_ui_system_terminal_set_focused(app->ui, true);
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.tab.prev") == 0) {
            if (sol_terminal_manager_count(mgr) == 0u) return false;
            sol_terminal_manager_prev_tab(mgr);
            sol_terminal_manager_set_visible(mgr, true);
            sol_ui_system_terminal_set_focused(app->ui, true);
            sol_ui_system_terminal_notify(app->ui);
            return true;
        }

        if (strcmp(p->action, "terminal.paste") == 0) {
            SolTerminal *term = sol_terminal_manager_active(mgr);
            if (!term) return false;
            Ca_Window *win = sol_ui_system_primary_window(app->ui);
            const char *text = win ? ca_clipboard_get_text(win) : NULL;
            if (!text || text[0] == '\0') return false;
            sol_terminal_paste(term, text, strlen(text));
            return true;
        }

        return false;
    }

    /* Unknown action — let other subscribers (plugins) try. */
    return false;
}

/* ------------------------------------------------------------------ */
/* Deferred (post-first-frame) init                                    */
/* ------------------------------------------------------------------ */

/*
 * Run the startup work that does not gate the first painted frame:
 * scanning and dynamically loading every plugin from disk, restoring the
 * saved theme/background effect (which may be plugin-provided), and
 * announcing SOL_EVENT_APP_STARTUP / SOL_EVENT_APP_READY.
 *
 * Called once, from inside the frame loop right after the first
 * successful ca_instance_tick() — so the window is already created and
 * showing a frame before this disk I/O and dynamic-library loading runs.
 *
 * app   The application context; app->deferred_init_done is set true
 *       before returning so the caller never runs this twice.
 * argc  Process argument count, forwarded from main() to resolve the
 *       plugin directory relative to argv[0].
 * argv  Process argument vector, forwarded from main().
 */
static void sol_run_deferred_init(SolAppContext *app, int argc, char **argv)
{
    app->deferred_init_done = true;

    char plugin_dir[1024] = "plugins";
    (void)sol_resolve_plugin_directory(
        argc > 0 ? argv[0] : NULL, plugin_dir, sizeof(plugin_dir));
    const uint32_t loaded_plugins =
        (uint32_t)sol_system_load_plugins_from_directory(app->systems, plugin_dir);

    /* Restore the saved CSS theme now that plugins have registered theirs. */
    if (app->settings.theme_id[0] != '\0' &&
        !sol_ui_system_set_active_theme(app->ui, app->settings.theme_id)) {
        snprintf(app->settings.theme_id, sizeof(app->settings.theme_id), "%s",
                 SOL_SETTINGS_THEME_ID_DEFAULT);
        if (!sol_ui_system_set_active_theme(app->ui, app->settings.theme_id)) {
            snprintf(app->settings.theme_id, sizeof(app->settings.theme_id), "%s",
                     SOL_SETTINGS_THEME_ID_FALLBACK);
            (void)sol_ui_system_set_active_theme(app->ui, app->settings.theme_id);
        }
    }
    /* Always apply appearance overlay after themes load — set_active_theme
     * skips the callback when the theme index hasn't changed, so the overlay
     * would otherwise be missing on startup. */
    sol_ui_system_apply_appearance(app->ui);

    /* Restore the saved background effect now that plugins have registered theirs. */
    if (app->bg_effects && app->settings.bg_effect_id[0] != '\0' &&
        !sol_bg_effect_set_active(app->bg_effects, app->settings.bg_effect_id)) {
        snprintf(app->settings.bg_effect_id, sizeof(app->settings.bg_effect_id), "%s",
                 SOL_SETTINGS_BG_EFFECT_ID_DEFAULT);
        if (!sol_bg_effect_set_active(app->bg_effects, app->settings.bg_effect_id))
            app->settings.bg_effect_id[0] = '\0';
    }

    const SolAppStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app->jobs),
        .loaded_plugins = loaded_plugins,
        .input_binding_active = app->command_flows_loaded > 0,
    };

    sol_event_bus_post(app->events, &(SolEventDesc){
        .event_name   = SOL_EVENT_APP_STARTUP,
        .payload      = &startup,
        .payload_size = sizeof(startup),
        .sender       = app->systems,
        .flags        = SOL_EVENT_FLAG_NONE,
    });
    sol_system_pump_events(app->systems, 16u);

    /* Window and subsystems are live — announce app readiness. Plugins
       can use this hook to perform first-frame setup that needs the
       window to exist. Payload is intentionally empty. */
    sol_event_publish(app->events, SOL_EVENT_APP_READY, NULL, 0u, app->systems);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    SolAppContext app;
    memset(&app, 0, sizeof(app));

    SolSystemConfig system_config = sol_system_config_default();
    app.systems = sol_system_manager_create(&system_config);
    if (!app.systems) {
        fprintf(stderr, "Failed to create system manager\n");
        return 1;
    }

    app.events  = sol_system_events(app.systems);
    app.buffers = sol_system_buffers(app.systems);
    app.jobs    = sol_system_jobs(app.systems);
    app.input   = sol_system_input(app.systems);

    /* CLI: `./sol <path>` — file → open buffer; dir → mount tree root. */
    const char *cli_path =
        (argc >= 2 && argv[1] && argv[1][0] != '\0') ? argv[1] : NULL;
    bool cli_is_dir = false;
    if (cli_path) {
        SolPathInfo info;
        if (!sol_platform_get_path_info(cli_path, &info)) {
            fprintf(stderr, "sol: cannot stat '%s'\n", cli_path);
            sol_system_manager_destroy(app.systems);
            return 1;
        }
        cli_is_dir = info.is_directory;
        /* Defer file opens until after the UI system exists so the
           render callback has somewhere to invalidate. */
    }

    app.startup_token = sol_event_bus_subscribe(app.events,
        &(SolEventSubscriptionDesc){
            .event_name = SOL_EVENT_APP_STARTUP,
            .priority   = 100,
            .handler    = sol_on_startup_event,
            .user_data  = NULL,
        });

    /* Load user settings before creating Causality so the global UI scale is
       present from instance init, before any windows inherit it. */
    sol_settings_load(&app.settings);

    /* Compiled-shader cache under ~/.sol — see Ca_InstanceDesc::shader_cache_dir.
       A NULL path (config dir unresolvable, e.g. $HOME unset) just leaves
       caching disabled for this run; Causality falls back to compiling
       every shader via shaderc exactly as before this existed. */
    char *shader_cache_dir = sol_config_path("shader_cache");

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .default_ui_scale     = app.settings.ui_scale,
        .shader_cache_dir     = shader_cache_dir,
    });
    free(shader_cache_dir);
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        sol_system_manager_destroy(app.systems);
        return 1;
    }
    app.instance = instance;

    /* Wire the buffer system to the bus BEFORE the UI is built so the
       UI system can in turn share the bus with the file tree. After
       this call, every buffer create/close/focus and every text edit
       fans out to subscribers. */
    sol_buffer_attach_event_bus(app.buffers, app.events);

    app.ui = sol_ui_system_create(instance, app.buffers);
    if (!app.ui) {
        fprintf(stderr, "Failed to create UI system\n");
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    /* Created before any explorer-root change (CLI dir arg or cwd
       fallback below) so sol_set_explorer_root can attach the watcher
       to the very first root, not just later folder-open actions. */
    app.watcher = sol_file_watcher_create();
    if (!app.watcher) {
        fprintf(stderr, "[sol] warning: file watcher creation failed; "
                        "explorer/buffers will not live-update on external changes\n");
    } else {
        sol_file_watcher_set_wake_callback(app.watcher, sol_on_file_watcher_wake, NULL);
    }

    /* Now safe to open the CLI file (renderer is wired up). */
    if (cli_path && !cli_is_dir) {
        if (!sol_open_path_in_active_leaf(&app, cli_path)) {
            sol_ui_system_destroy(app.ui);
            ca_instance_destroy(instance);
            sol_system_manager_destroy(app.systems);
            return 1;
        }
    }

    sol_ui_system_set_file_open_callback(app.ui, sol_on_tree_file_open, &app);
    sol_ui_system_set_focus_region_callback(app.ui, sol_on_ui_focus_region, &app);
    sol_ui_system_set_terminal_focus_gain_callback(app.ui, sol_on_terminal_focus_gain, &app);
    sol_ui_system_set_context_action_callback(app.ui, sol_on_context_action, &app);
    if (cli_is_dir && cli_path) {
        if (!sol_set_explorer_root(&app, cli_path)) {
            fprintf(stderr, "sol: cannot open directory '%s'\n", cli_path);
        }
    } else if (!cli_path) {
        /* No CLI argument: open the working directory in the explorer so
           the panel is visible on first launch. */
        char cwd_buf[4096];
        if (sol_platform_get_cwd(cwd_buf, sizeof(cwd_buf))) {
            if (!sol_set_explorer_root(&app, cwd_buf)) {
                fprintf(stderr, "sol: cannot open cwd as explorer root\n");
            }
        }
    }

    sol_ui_system_install_menu(app.ui,
                               sol_on_menu_new_buffer,
                               sol_on_menu_open_file,
                               sol_on_menu_open_folder,
                               &app);

    /* Subscribe to command-invoked events BEFORE loading the bindings
       file so any chord that happens to fire during early startup
       (none today, but plugins might queue one) is observed. The
       subscriber dispatches built-in actions; plugins may install
       their own subscribers for additional actions. */
    app.command_token = sol_event_bus_subscribe(app.events,
        &(SolEventSubscriptionDesc){
            .event_name = SOL_EVENT_COMMAND_INVOKED,
            .priority   = 0,
            .handler    = sol_on_command_invoked,
            .user_data  = &app,
        });

    app.text_edited_token = sol_event_bus_subscribe(app.events,
        &(SolEventSubscriptionDesc){
            .event_name = SOL_EVENT_TEXT_EDITED,
            .priority   = 0,
            .handler    = sol_on_text_edited_for_autosave,
            .user_data  = &app,
        });

    sol_register_search_command_defaults(app.ui);
    sol_register_terminal_command_defaults(app.ui);
    sol_register_buffer_save_command_defaults(app.ui);
    app.command_flows_loaded = sol_config_load_bindings(app.ui);
    if (app.command_flows_loaded < 0) {
        fprintf(stderr, "sol: failed to load key bindings from ~/.sol/bindings.conf\n");
        app.command_flows_loaded = 0;
    }
    sol_register_workspace_menu_items(app.ui);

    app.router = sol_input_router_create(instance, app.ui, app.input, app.buffers);
    if (!app.router) {
        fprintf(stderr, "Failed to create input router\n");
        sol_ui_system_destroy(app.ui);
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    app.terminal_mgr = sol_terminal_manager_create(instance);
    if (!app.terminal_mgr) {
        fprintf(stderr, "[sol] warning: terminal manager creation failed; terminal unavailable\n");
    } else {
        sol_ui_system_set_terminal_manager(app.ui, app.terminal_mgr);
    }

    app.bg_effects = sol_bg_effect_registry_create(instance);
    if (!app.bg_effects) {
        fprintf(stderr, "[sol] warning: background effect registry creation failed\n");
    } else {
        sol_bg_effect_set_opacity(app.bg_effects, app.settings.bg_opacity);
        sol_ui_system_set_bg_effects(app.ui, app.bg_effects);
    }

    Ca_Window *window = sol_ui_system_primary_window(app.ui);
    if (!window) {
        fprintf(stderr, "Failed to access primary window\n");
        sol_input_router_destroy(app.router);
        sol_ui_system_destroy(app.ui);
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    if (app.terminal_mgr) {
        sol_terminal_manager_set_clipboard_write(
            app.terminal_mgr, sol_on_terminal_clipboard_write, window);
    }

    if (!sol_system_register_service(app.systems, "ca.instance", instance, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.instance service\n");
    }
    if (!sol_system_register_service(app.systems, "ca.window.primary", window, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.window.primary service\n");
    }
    if (!sol_system_register_service(app.systems, "sol.ui", app.ui, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register sol.ui service\n");
    }
    if (app.bg_effects &&
        !sol_system_register_service(app.systems, "sol.bg_effect_registry",
                                     app.bg_effects, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register sol.bg_effect_registry service\n");
    }
    sol_plugin_manager_attach_ui(sol_system_plugins(app.systems), app.ui);
    sol_ui_system_set_plugin_manager(app.ui, sol_system_plugins(app.systems));
    sol_ui_system_set_settings(app.ui, &app.settings);

    app.syntax_registry = sol_syntax_registry_create();
    sol_syntax_set_global_registry(app.syntax_registry);
    sol_plugin_manager_attach_syntax_registry(
        sol_system_plugins(app.systems), app.syntax_registry);

    for (;;) {
        sol_system_begin_frame(app.systems);
        sol_ui_system_pre_tick(app.ui);
        if (!ca_instance_tick(instance)) break;
        sol_system_pump_events(app.systems, 128u);
        if (!app.deferred_init_done) {
            sol_run_deferred_init(&app, argc, argv);
        }
        sol_drain_file_watcher(&app);
        sol_run_autosave_sweep(&app);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }
    if (app.command_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.command_token);
    }
    if (app.text_edited_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.text_edited_token);
    }
    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    /* Stop the watcher's background thread before anything its drain
       path touches (event bus, buffers, UI) is torn down — same
       ordering requirement as the terminal manager's PTY reader below. */
    sol_file_watcher_destroy(app.watcher);
    app.watcher = NULL;

    sol_input_router_destroy(app.router);
    /* Plugins must quiesce and unregister while the UI, syntax registry, and
       Causality instance they reference are still alive. */
    SolPluginManager *plugins = sol_system_plugins(app.systems);
    if (plugins) {
        (void)sol_plugin_manager_unload_all(plugins);
        sol_plugin_manager_attach_ui(plugins, NULL);
    }
    sol_ui_system_set_plugin_manager(app.ui, NULL);
    sol_system_unregister_service(app.systems, "sol.ui");
    sol_system_unregister_service(app.systems, "sol.bg_effect_registry");
    /* Destroy the terminal manager before the UI system so PTY reader threads
       stop before causality signals are freed. */
    sol_ui_system_set_terminal_manager(app.ui, NULL);
    sol_terminal_manager_destroy(app.terminal_mgr);
    /* Detach and destroy bg effects before UI destroy so Vulkan pipelines are
       freed before the Causality instance tears down its device. */
    sol_ui_system_set_bg_effects(app.ui, NULL);
    sol_bg_effect_registry_destroy(app.bg_effects);
    sol_ui_system_destroy(app.ui);
    sol_syntax_registry_destroy(app.syntax_registry);
    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
