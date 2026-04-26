#include <causality.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sol_buffer.h"
#include "sol_ui_constants.h"
#include "sol_system_manager.h"
#include "sol_ui_system.h"

typedef struct SolStartupPayload {
    uint32_t worker_count;
    uint32_t loaded_plugins;
    uint64_t warmup_checksum;
    bool input_binding_active;
} SolStartupPayload;

typedef struct SolWarmupContext {
    _Atomic uint64_t checksum;
} SolWarmupContext;

typedef struct SolAppContext {
    SolSystemManager *systems;
    SolEventBus *events;
    SolBufferSystem *buffers;
    SolJobSystem *jobs;
    SolInputSystem *input;
    SolSubscriptionToken startup_token;
    bool command_flows_ready;
    SolUISystem *ui;
} SolAppContext;

typedef struct SolTextBufferState {
    char *text;
} SolTextBufferState;

static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, value, len + 1u);
    return copy;
}

static void sol_text_buffer_destroy(void *state)
{
    SolTextBufferState *text_state = (SolTextBufferState *)state;
    if (!text_state) {
        return;
    }

    free(text_state->text);
    free(text_state);
}

static void sol_text_buffer_render(const SolBuffer *buffer, const SolBufferRenderArgs *args, void *state)
{
    (void)buffer;
    (void)args;

    const SolTextBufferState *text_state = (const SolTextBufferState *)state;

    ca_text(&(Ca_TextDesc){
        .text = text_state && text_state->text ? text_state->text : "",
        .style = "buffer-body-text",
    });
}

static SolBufferId sol_create_text_buffer(SolBufferSystem *buffers, const char *name, const char *text)
{
    if (!buffers) {
        return 0u;
    }

    SolTextBufferState *state = (SolTextBufferState *)calloc(1u, sizeof(SolTextBufferState));
    if (!state) {
        return 0u;
    }

    state->text = sol_strdup(text ? text : "");
    if (!state->text) {
        free(state);
        return 0u;
    }

    SolBufferDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = name;
    desc.kind = SOL_BUFFER_KIND_TEXT;
    desc.state = state;
    desc.ops.destroy = sol_text_buffer_destroy;
    desc.ops.render = sol_text_buffer_render;

    const SolBufferId id = sol_buffer_create(buffers, &desc);
    if (id == 0u) {
        sol_text_buffer_destroy(state);
    }

    return id;
}

static SolModifierMask sol_modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if ((mods & 0x0001) != 0) {
        out |= SOL_MOD_SHIFT;
    }
    if ((mods & 0x0002) != 0) {
        out |= SOL_MOD_CTRL;
    }
    if ((mods & 0x0004) != 0) {
        out |= SOL_MOD_ALT;
    }
    if ((mods & 0x0008) != 0) {
        out |= SOL_MOD_SUPER;
    }
    return out;
}

static bool sol_on_startup_event(const SolEvent *event, void *user_data)
{
    (void)user_data;

    if (!event || !event->payload || event->payload_size != sizeof(SolStartupPayload)) {
        return false;
    }

    const SolStartupPayload *payload = (const SolStartupPayload *)event->payload;
    printf(
        "[sol] startup: workers=%u plugins=%u warmup=%llu input=%s\n",
        payload->worker_count,
        payload->loaded_plugins,
        (unsigned long long)payload->warmup_checksum,
        payload->input_binding_active ? "ready" : "missing"
    );

    return false;
}

static void sol_warmup_range(uint32_t begin, uint32_t end, void *user_data)
{
    SolWarmupContext *context = (SolWarmupContext *)user_data;
    uint64_t local_sum = 0u;

    for (uint32_t i = begin; i < end; ++i) {
        local_sum += ((uint64_t)i * 2654435761ull) ^ ((uint64_t)i >> 3u);
    }

    atomic_fetch_add_explicit(&context->checksum, local_sum, memory_order_relaxed);
}

static void sol_on_ca_key(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->key.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_KEY_UP;
    } else if (ev->key.action == CA_PRESS || ev->key.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_KEY_DOWN;
    } else {
        return;
    }

    input_event.data.key.key = (SolKeyCode)ev->key.key;
    input_event.data.key.modifiers = sol_modifiers_from_ca(ev->key.mods);
    input_event.data.key.repeated = (ev->key.action == CA_REPEAT);

    sol_ui_system_handle_input_event(app->ui, &input_event);
    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_char(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_TEXT_INPUT;
    input_event.data.text.codepoint = ev->character.codepoint;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_button(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->mouse_button.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_UP;
    } else if (ev->mouse_button.action == CA_PRESS || ev->mouse_button.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_DOWN;
    } else {
        return;
    }

    input_event.data.mouse_button.button = (SolMouseButton)ev->mouse_button.button;
    input_event.data.mouse_button.modifiers = sol_modifiers_from_ca(ev->mouse_button.mods);
    input_event.data.mouse_button.repeated = (ev->mouse_button.action == CA_REPEAT);

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_move(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_MOVE;
    input_event.data.mouse_move.x = ev->mouse_pos.x;
    input_event.data.mouse_move.y = ev->mouse_pos.y;
    input_event.data.mouse_move.delta_x = 0.0;
    input_event.data.mouse_move.delta_y = 0.0;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_SCROLL;
    input_event.data.mouse_scroll.x = (float)ev->mouse_scroll.dx;
    input_event.data.mouse_scroll.y = (float)ev->mouse_scroll.dy;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_window_close(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_close(app->ui, ev->window);
}

static void sol_on_ca_window_resize(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_resize(app->ui, ev->resize.width, ev->resize.height);
}

int main(void)
{
    SolAppContext app;
    memset(&app, 0, sizeof(app));

    SolSystemConfig system_config = sol_system_config_default();
    app.systems = sol_system_manager_create(&system_config);
    if (!app.systems) {
        fprintf(stderr, "Failed to create system manager\n");
        return 1;
    }

    app.events = sol_system_events(app.systems);
    app.buffers = sol_system_buffers(app.systems);
    app.jobs = sol_system_jobs(app.systems);
    app.input = sol_system_input(app.systems);

    const SolBufferId welcome_buffer = sol_create_text_buffer(
        app.buffers,
        "main",
        ""
    );
    if (welcome_buffer == 0u) {
        fprintf(stderr, "Failed to create initial buffer\n");
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    app.startup_token = sol_event_bus_subscribe(app.events, &(SolEventSubscriptionDesc){
        .event_name = "core.startup",
        .priority = 100,
        .handler = sol_on_startup_event,
        .user_data = NULL,
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

    app.ui = sol_ui_system_create(instance, app.buffers);
    if (!app.ui) {
        fprintf(stderr, "Failed to create UI system\n");
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    static const SolKeyCode flow_editor_save[] = { 'F', 'S' };
    static const SolKeyCode flow_workspace_split_vertical[] = { 'W', 'V' };
    static const SolKeyCode flow_workspace_split_horizontal[] = { 'W', 'H' };
    static const SolKeyCode flow_workspace_focus_next[] = { 'W', 'N' };

    const bool save_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "editor.save",
        .label = "Save",
        .sequence = flow_editor_save,
        .sequence_length = 2u,
        .key = 'S',
        .callback = sol_ui_system_on_save_action,
        .user_data = app.ui,
    });

    const bool split_vertical_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.vertical",
        .label = "Split Vertical",
        .sequence = flow_workspace_split_vertical,
        .sequence_length = 2u,
        .key = 'V',
        .callback = sol_ui_system_on_split_vertical_action,
        .user_data = app.ui,
    });

    const bool split_horizontal_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.horizontal",
        .label = "Split Horizontal",
        .sequence = flow_workspace_split_horizontal,
        .sequence_length = 2u,
        .key = 'H',
        .callback = sol_ui_system_on_split_horizontal_action,
        .user_data = app.ui,
    });

    const bool focus_next_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.focus.next",
        .label = "Focus Next Pane",
        .sequence = flow_workspace_focus_next,
        .sequence_length = 2u,
        .key = 'N',
        .callback = sol_ui_system_on_focus_next_action,
        .user_data = app.ui,
    });

    app.command_flows_ready = save_flow && split_vertical_flow && split_horizontal_flow && focus_next_flow;

    ca_event_set_handler(instance, CA_EVENT_KEY, sol_on_ca_key, &app);
    ca_event_set_handler(instance, CA_EVENT_CHAR, sol_on_ca_char, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON, sol_on_ca_mouse_button, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE, sol_on_ca_mouse_move, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL, sol_on_ca_mouse_scroll, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE, sol_on_ca_window_close, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, sol_on_ca_window_resize, &app);

    Ca_Window *window = sol_ui_system_primary_window(app.ui);
    if (!window) {
        fprintf(stderr, "Failed to access primary window\n");
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

    SolWarmupContext warmup = { 0 };
    bool warmup_ok = sol_job_system_parallel_for(app.jobs, 100000u, 256u, sol_warmup_range, &warmup);

    const uint32_t loaded_plugins = (uint32_t)sol_system_load_plugins_from_directory(app.systems, NULL);

    const SolStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app.jobs),
        .loaded_plugins = loaded_plugins,
        .warmup_checksum = warmup_ok ? atomic_load_explicit(&warmup.checksum, memory_order_relaxed) : 0u,
        .input_binding_active = app.command_flows_ready,
    };

    sol_event_bus_post(app.events, &(SolEventDesc){
        .event_name = "core.startup",
        .payload = &startup,
        .payload_size = sizeof(startup),
        .sender = app.systems,
        .flags = SOL_EVENT_FLAG_NONE,
    });
    sol_system_pump_events(app.systems, 16u);

    for (;;) {
        sol_system_begin_frame(app.systems);
        if (!ca_instance_tick(instance)) {
            break;
        }
        sol_system_pump_events(app.systems, 128u);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }

    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    sol_ui_system_destroy(app.ui);

    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
