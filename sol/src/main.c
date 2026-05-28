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
 *   4. Register the leader-key command flows (Save / Split / Cycle).
 *   5. Drive the frame loop.
 *
 * Everything heavier — text storage, rendering, input dispatch — lives
 * in dedicated modules:
 *
 *   sol/src/core/sol_text_buffer.c   rope-backed editing primitives
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
#include <sys/stat.h>

#include "sol_buffer.h"
#include "sol_event.h"
#include "sol_file_picker.h"
#include "sol_input.h"
#include "sol_input_router.h"
#include "sol_job.h"
#include "sol_system_manager.h"
#include "sol_text_buffer.h"
#include "sol_text_view.h"
#include "sol_ui_constants.h"
#include "sol_ui_system.h"
#include "sol_event.h"

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
    bool                  command_flows_ready;
    SolUISystem          *ui;
    Ca_Instance          *instance;
    SolInputRouter       *router;
} SolAppContext;

/* ------------------------------------------------------------------ */
/* Buffer-open glue                                                    */
/* ------------------------------------------------------------------ */

/* Open `path` into the active leaf, deduping against an existing
   buffer with the same source path. */
static bool sol_open_path_in_active_leaf(SolAppContext *app, const char *path)
{
    if (!app || !app->buffers || !path) return false;

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
    if (!sol_ui_system_set_file_tree_root(app->ui, path)) {
        fprintf(stderr, "sol: cannot open directory '%s'\n", path);
    }
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
/* Command-flow registration (leader-key actions)                      */
/* ------------------------------------------------------------------ */

static bool sol_register_command_flows(SolUISystem *ui)
{
    static const SolKeyCode flow_editor_save[]                = { 'F', 'S' };
    static const SolKeyCode flow_workspace_split_vertical[]   = { 'W', 'V' };
    static const SolKeyCode flow_workspace_split_horizontal[] = { 'W', 'H' };
    static const SolKeyCode flow_workspace_focus_next[]       = { 'W', 'N' };
    static const SolKeyCode flow_buffer_next[]                = { 'B', 'D' };
    static const SolKeyCode flow_buffer_prev[]                = { 'B', 'A' };

    const struct {
        const char        *action;
        const char        *label;
        const SolKeyCode  *seq;
        SolKeyCode         key;
        SolInputActionCallback cb;
    } flows[] = {
        { "editor.save",                "Save",                flow_editor_save,                'S', sol_ui_system_on_save_action },
        { "workspace.split.vertical",   "Split Vertical",      flow_workspace_split_vertical,   'V', sol_ui_system_on_split_vertical_action },
        { "workspace.split.horizontal", "Split Horizontal",    flow_workspace_split_horizontal, 'H', sol_ui_system_on_split_horizontal_action },
        { "workspace.focus.next",       "Focus Next Pane",     flow_workspace_focus_next,       'N', sol_ui_system_on_focus_next_action },
        { "buffer.next",                "Next Buffer",         flow_buffer_next,                'D', sol_ui_system_on_buffer_next_action },
        { "buffer.prev",                "Previous Buffer",     flow_buffer_prev,                'A', sol_ui_system_on_buffer_prev_action },
    };

    bool all_ok = true;
    for (size_t i = 0u; i < sizeof(flows) / sizeof(flows[0]); ++i) {
        const bool ok = sol_ui_system_register_command_flow(ui,
            &(SolCommandFlowDesc){
                .action          = flows[i].action,
                .label           = flows[i].label,
                .sequence        = flows[i].seq,
                .sequence_length = 2u,
                .key             = flows[i].key,
                .callback        = flows[i].cb,
                .user_data       = ui,
            });
        all_ok = all_ok && ok;
    }
    return all_ok;
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
        struct stat st;
        if (stat(cli_path, &st) != 0) {
            fprintf(stderr, "sol: cannot stat '%s'\n", cli_path);
            sol_system_manager_destroy(app.systems);
            return 1;
        }
        cli_is_dir = S_ISDIR(st.st_mode);
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
        .font_size_px         = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT,
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
    if (cli_is_dir && cli_path) {
        if (!sol_ui_system_set_file_tree_root(app.ui, cli_path)) {
            fprintf(stderr, "sol: cannot open directory '%s'\n", cli_path);
        }
    }

    sol_ui_system_install_menu(app.ui,
                               sol_on_menu_open_file,
                               sol_on_menu_open_folder,
                               &app);

    app.command_flows_ready = sol_register_command_flows(app.ui);

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

    SolWarmupContext warmup = {0};
    const bool warmup_ok = sol_job_system_parallel_for(
        app.jobs, 100000u, 256u, sol_warmup_range, &warmup);

    const uint32_t loaded_plugins =
        (uint32_t)sol_system_load_plugins_from_directory(app.systems, NULL);

    const SolAppStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app.jobs),
        .loaded_plugins = loaded_plugins,
        .warmup_checksum = warmup_ok
            ? atomic_load_explicit(&warmup.checksum, memory_order_relaxed)
            : 0u,
        .input_binding_active = app.command_flows_ready,
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
        if (!ca_instance_tick(instance)) break;
        sol_system_pump_events(app.systems, 128u);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }
    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    sol_input_router_destroy(app.router);
    sol_ui_system_destroy(app.ui);
    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
