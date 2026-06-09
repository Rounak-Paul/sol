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

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_buffer.h"
#include "sol_config.h"
#include "sol_event.h"
#include "sol_file_picker.h"
#include "sol_input.h"
#include "sol_input_router.h"
#include "sol_job.h"
#include "sol_platform.h"
#include "sol_settings.h"
#include "sol_system_manager.h"
#include "sol_syntax.h"
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

typedef struct SolWarmupContext {
    _Atomic uint64_t checksum;
} SolWarmupContext;

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
    Ca_Instance          *instance;
    SolInputRouter       *router;
    bool                  explorer_focused;
    SolBufferNodeId       focus_before_explorer;
    char                  file_clipboard_path[4096];
    bool                  file_clipboard_cut;
    SolSettings           settings;
} SolAppContext;

typedef struct SolDeletePathRequest {
    SolAppContext *app;
    char path[4096];
} SolDeletePathRequest;

/* ------------------------------------------------------------------ */
/* Buffer-open glue                                                    */
/* ------------------------------------------------------------------ */

static bool sol_set_explorer_root(SolAppContext *app, const char *path)
{
    if (!app || !app->ui) return false;
    return sol_ui_system_set_file_tree_root(app->ui, path);
}

static int sol_active_buffer_viewport_lines(SolAppContext *app, SolTextBuffer *tb)
{
    if (!app || !app->ui || !app->buffers || !tb) {
        return 1;
    }

    Ca_Window *win = sol_ui_system_primary_window(app->ui);
    const float scale = win ? ca_window_get_scale(win) : 1.0f;
    SolBufferRect root_rect = {0};
    if (sol_ui_system_buffer_area_rect(app->ui, &root_rect.x, &root_rect.y,
                                       &root_rect.w, &root_rect.h)) {
        SolBufferRect leaf_rect = {0};
        const SolBufferNodeId leaf = sol_buffer_active_leaf(app->buffers);
        if (leaf != 0u &&
            sol_buffer_leaf_geometry(app->buffers, leaf, &root_rect, 1.0f, &leaf_rect)) {
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

static void sol_refresh_explorer(SolAppContext *app)
{
    if (!app || !app->ui) return;
    const char *root = sol_ui_system_file_tree_root(app->ui);
    if (root) {
        char root_copy[4096];
        const size_t len = strlen(root);
        if (len < sizeof(root_copy)) {
            memcpy(root_copy, root, len + 1u);
            (void)sol_ui_system_set_file_tree_root(app->ui, root_copy);
        }
    }
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
    printf("[sol] startup: workers=%u plugins=%u warmup=%llu input=%s\n",
           p->worker_count, p->loaded_plugins,
           (unsigned long long)p->warmup_checksum,
           p->input_binding_active ? "ready" : "missing");
    return false;
}

static void sol_warmup_range(uint32_t begin, uint32_t end, void *user_data)
{
    SolWarmupContext *ctx = (SolWarmupContext *)user_data;
    uint64_t local = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        local += ((uint64_t)i * 2654435761ull) ^ ((uint64_t)i >> 3u);
    }
    atomic_fetch_add_explicit(&ctx->checksum, local, memory_order_relaxed);
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

static bool sol_toggle_explorer_focus(SolAppContext *app)
{
    if (!app || !app->ui || !app->buffers) return false;

    if (app->explorer_focused) {
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

    if (sol_ui_system_tree_panel_width(app->ui) == 0) {
        const char *root = sol_ui_system_file_tree_root(app->ui);
        char cwd_buf[4096];
        if (!root || root[0] == '\0') {
            if (!sol_platform_get_cwd(cwd_buf, sizeof(cwd_buf))) {
                return false;
            }
            root = cwd_buf;
            if (!sol_set_explorer_root(app, root)) {
                return false;
            }
        } else {
            sol_ui_system_set_file_tree_visible(app->ui, true);
        }
    }

    /* Capture exactly once per explorer-focus session: the last thing
       focused before entering explorer. */
    app->focus_before_explorer = sol_buffer_active_leaf(app->buffers);
    app->explorer_focused = true;
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

    /* ---- buffer.focus.* */
    if (strcmp(p->action, "buffer.focus.previous") == 0) {
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

    /* Unknown action — let other subscribers (plugins) try. */
    return false;
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

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .default_ui_scale     = app.settings.ui_scale,
    });
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

    sol_register_search_command_defaults(app.ui);
    app.command_flows_loaded = sol_config_load_bindings(app.ui);
    if (app.command_flows_loaded < 0) {
        fprintf(stderr, "sol: failed to load key bindings from ~/.sol/bindings.conf\n");
        app.command_flows_loaded = 0;
    }

    app.router = sol_input_router_create(instance, app.ui, app.input, app.buffers);
    if (!app.router) {
        fprintf(stderr, "Failed to create input router\n");
        sol_ui_system_destroy(app.ui);
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
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

    if (!sol_system_register_service(app.systems, "ca.instance", instance, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.instance service\n");
    }
    if (!sol_system_register_service(app.systems, "ca.window.primary", window, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.window.primary service\n");
    }
    if (!sol_system_register_service(app.systems, "sol.ui", app.ui, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register sol.ui service\n");
    }
    sol_plugin_manager_attach_ui(sol_system_plugins(app.systems), app.ui);
    sol_ui_system_set_plugin_manager(app.ui, sol_system_plugins(app.systems));
    sol_ui_system_set_settings(app.ui, &app.settings);

    app.syntax_registry = sol_syntax_registry_create();
    sol_syntax_set_global_registry(app.syntax_registry);
    sol_plugin_manager_attach_syntax_registry(
        sol_system_plugins(app.systems), app.syntax_registry);

    SolWarmupContext warmup = {0};
    const bool warmup_ok = sol_job_system_parallel_for(
        app.jobs, 100000u, 256u, sol_warmup_range, &warmup);

    char plugin_dir[1024] = "plugins";
    (void)sol_resolve_plugin_directory(
        argc > 0 ? argv[0] : NULL, plugin_dir, sizeof(plugin_dir));
    const uint32_t loaded_plugins =
        (uint32_t)sol_system_load_plugins_from_directory(app.systems, plugin_dir);

    const SolAppStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app.jobs),
        .loaded_plugins = loaded_plugins,
        .warmup_checksum = warmup_ok
            ? atomic_load_explicit(&warmup.checksum, memory_order_relaxed)
            : 0u,
        .input_binding_active = app.command_flows_loaded > 0,
    };

    sol_event_bus_post(app.events, &(SolEventDesc){
        .event_name   = SOL_EVENT_APP_STARTUP,
        .payload      = &startup,
        .payload_size = sizeof(startup),
        .sender       = app.systems,
        .flags        = SOL_EVENT_FLAG_NONE,
    });
    sol_system_pump_events(app.systems, 16u);

    /* Window and subsystems are live — announce app readiness. Plugins
       can use this hook to perform first-frame setup that needs the
       window to exist. Payload is intentionally empty. */
    sol_event_publish(app.events, SOL_EVENT_APP_READY, NULL, 0u, app.systems);

    for (;;) {
        sol_system_begin_frame(app.systems);
        sol_ui_system_pre_tick(app.ui);
        if (!ca_instance_tick(instance)) break;
        sol_system_pump_events(app.systems, 128u);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }
    if (app.command_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.command_token);
    }
    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    sol_input_router_destroy(app.router);
    sol_ui_system_destroy(app.ui);
    sol_syntax_registry_destroy(app.syntax_registry);
    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
