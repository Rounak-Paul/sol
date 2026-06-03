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
    SolSettings           settings;
} SolAppContext;

/* ------------------------------------------------------------------ */
/* Buffer-open glue                                                    */
/* ------------------------------------------------------------------ */

static bool sol_set_explorer_root(SolAppContext *app, const char *path)
{
    if (!app || !app->ui) return false;
    return sol_ui_system_set_file_tree_root(app->ui, path);
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
        int win_h = 0;
        sol_ui_system_window_size(app->ui, NULL, &win_h);
        if (win_h <= 0) win_h = 600;
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        const float sc = ca_window_get_scale(win);
        int vp = sol_text_view_visible_lines(win_h, sc) - 2;
        if (vp < 1) vp = 1;
        sol_text_buffer_ensure_cursor_visible(tb, vp);
        sol_ui_system_invalidate_buffer_area(app->ui);
        return true;
    }

    /* ---- edit.redo --------------------------------------------------------- */
    if (strcmp(p->action, "edit.redo") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb || !sol_text_buffer_can_redo(tb)) return false;
        sol_text_buffer_redo(tb);
        int win_h = 0;
        sol_ui_system_window_size(app->ui, NULL, &win_h);
        if (win_h <= 0) win_h = 600;
        Ca_Window *win = sol_ui_system_primary_window(app->ui);
        const float sc = ca_window_get_scale(win);
        int vp = sol_text_view_visible_lines(win_h, sc) - 2;
        if (vp < 1) vp = 1;
        sol_text_buffer_ensure_cursor_visible(tb, vp);
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

    /* ---- edit.delete_char : delete one character forward (or selection). --- */
    if (strcmp(p->action, "edit.delete_char") == 0) {
        SolTextBuffer *tb = sol_text_buffer_active(app->buffers);
        if (!tb) return false;
        const bool changed = sol_text_buffer_delete_forward(tb);
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

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
    });
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        sol_system_manager_destroy(app.systems);
        return 1;
    }
    app.instance = instance;

    /* Load user settings and apply initial scale. */
    sol_settings_load(&app.settings);
    ca_instance_set_scale(instance, app.settings.ui_scale);

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

    /* Resolve plugin directory relative to the executable so the app works
     * regardless of the current working directory.
     * argv[0] = ".../bin/sol"  → plugin dir = ".../bin/plugins"          */
    char plugin_dir[1024] = "bin/plugins";  /* fallback if argv[0] unusable */
    if (argv[0] && argv[0][0] != '\0') {
        const char *sep = strrchr(argv[0], '/');
        if (sep) {
            size_t dir_len = (size_t)(sep - argv[0]);
            if (dir_len + sizeof("/plugins") < sizeof(plugin_dir)) {
                memcpy(plugin_dir, argv[0], dir_len);
                memcpy(plugin_dir + dir_len, "/plugins", sizeof("/plugins"));
            }
        }
    }
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
