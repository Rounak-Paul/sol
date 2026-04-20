#include "sol_ui_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "style.h"

typedef struct SolSaveState {
    uint32_t count;
} SolSaveState;

typedef struct SolWorkspaceVisitorContext {
    SolUISystem *ui;
} SolWorkspaceVisitorContext;

struct SolUISystem {
    Ca_Instance *instance;
    Ca_Window *primary_window;
    SolBufferSystem *buffers;

    Ca_Stylesheet *stylesheet;
    Ca_State *save_state;

    Ca_Div *workspace_host;
    Ca_Label *status_label;
    bool workspace_dirty;
};

static void sol_ui_mark_workspace_dirty(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    ui->workspace_dirty = true;
}

static int sol_ui_split_direction_to_ca(SolBufferSplitDirection direction)
{
    if (direction == SOL_BUFFER_SPLIT_VERTICAL) {
        return CA_HORIZONTAL;
    }
    return CA_VERTICAL;
}

static void sol_ui_visit_begin_split(SolBufferSplitDirection direction, float ratio, void *user_data)
{
    (void)ratio;

    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    ca_split_begin(&(Ca_SplitDesc){
        .direction = sol_ui_split_direction_to_ca(direction),
        .ratio = ratio,
        .bar_size = 5.0f,
        .bar_color = ca_color(0.22f, 0.24f, 0.30f, 1.0f),
        .bar_hover_color = ca_color(0.34f, 0.37f, 0.47f, 1.0f),
    });
}

static void sol_ui_visit_end_split(void *user_data)
{
    (void)user_data;
    ca_split_end();
}

static void sol_ui_visit_render_leaf(SolBuffer *buffer, SolBufferNodeId leaf_id, bool is_active, void *user_data)
{
    (void)leaf_id;

    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = is_active ? "buffer-pane buffer-pane-active" : "buffer-pane",
    });

    if (buffer) {
        const char *name = sol_buffer_name(buffer);
        if (!name) {
            name = "[No Name]";
        }

        ca_text(&(Ca_TextDesc){
            .text = name,
            .style = "buffer-name",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-body",
        });

        SolBufferRenderArgs args;
        args.is_active = is_active;
        args.ui_context = ctx->ui;
        sol_buffer_render(buffer, &args);

        ca_div_end();
    } else {
        ca_text(&(Ca_TextDesc){
            .text = "Empty buffer",
            .style = "buffer-body-text",
        });
    }

    ca_div_end();
}

static void sol_ui_render_workspace_tree(SolUISystem *ui)
{
    if (!ui || !ui->buffers) {
        return;
    }

    if (sol_buffer_count(ui->buffers) == 0u) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-pane",
        });

        ca_text(&(Ca_TextDesc){
            .text = "No buffers loaded",
            .style = "buffer-body-text",
        });

        ca_div_end();
        return;
    }

    SolWorkspaceVisitorContext visitor_context;
    visitor_context.ui = ui;

    SolBufferWorkspaceVisitor visitor;
    memset(&visitor, 0, sizeof(visitor));
    visitor.begin_split = sol_ui_visit_begin_split;
    visitor.end_split = sol_ui_visit_end_split;
    visitor.render_leaf = sol_ui_visit_render_leaf;

    sol_buffer_workspace_visit(ui->buffers, &visitor, &visitor_context);
}

static void sol_ui_rebuild_workspace(SolUISystem *ui)
{
    if (!ui || !ui->workspace_host) {
        return;
    }

    ca_div_clear(ui->workspace_host);
    sol_ui_render_workspace_tree(ui);
    ca_div_end();

    ui->workspace_dirty = false;
}

static void sol_ui_on_save_state_changed(const void *value, void *user_data)
{
    if (!value || !user_data) {
        return;
    }

    const SolSaveState *state = (const SolSaveState *)value;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui->status_label) {
        return;
    }

    char line[192];
    snprintf(
        line,
        sizeof(line),
        "Saves: %u  |  Ctrl+V vsplit  |  Ctrl+H split  |  Ctrl+W next pane",
        state->count
    );
    ca_set_text(ui->status_label, line);
}

static void sol_ui_on_frame(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->workspace_dirty) {
        return;
    }

    sol_ui_rebuild_workspace(ui);
}

static bool sol_ui_build_layout(SolUISystem *ui)
{
    if (!ui || !ui->primary_window) {
        return false;
    }

    ca_ui_begin(ui->primary_window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "app-root",
    });

    ca_h1(&(Ca_TextDesc){
        .text = "Sol",
        .style = "title",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Everything is a buffer. Split panes to compose your workspace.",
        .style = "subtitle",
    });

    ui->workspace_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "workspace-host",
    });

    sol_ui_render_workspace_tree(ui);
    ca_div_end();

    ui->status_label = ca_text(&(Ca_TextDesc){
        .text = "Saves: 0  |  Ctrl+V vsplit  |  Ctrl+H split  |  Ctrl+W next pane",
        .style = "status-line",
    });

    ca_ui_end();
    return true;
}

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers)
{
    if (!instance || !buffers) {
        return NULL;
    }

    SolUISystem *ui = (SolUISystem *)calloc(1u, sizeof(SolUISystem));
    if (!ui) {
        return NULL;
    }

    ui->instance = instance;
    ui->buffers = buffers;
    ui->workspace_dirty = true;

    ui->stylesheet = ca_css_parse(SOL_UI_MAIN_WINDOW_CSS);
    if (ui->stylesheet) {
        ca_instance_set_stylesheet(instance, ui->stylesheet);
    }

    ui->primary_window = ca_window_create(instance, &(Ca_WindowDesc){
        .title = "Sol",
        .width = 1080,
        .height = 720,
    });

    if (!ui->primary_window) {
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    SolSaveState initial = { 0u };
    ui->save_state = ca_state_create(instance, sizeof(SolSaveState), &initial);
    if (ui->save_state) {
        ca_state_observe(ui->save_state, sol_ui_on_save_state_changed, ui);
    }

    if (!sol_ui_build_layout(ui)) {
        if (ui->save_state) {
            ca_state_destroy(ui->save_state);
            ui->save_state = NULL;
        }
        ca_window_destroy(ui->primary_window);
        ui->primary_window = NULL;
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    ca_window_set_on_frame(ui->primary_window, sol_ui_on_frame, ui);
    return ui;
}

void sol_ui_system_destroy(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    if (ui->save_state) {
        ca_state_destroy(ui->save_state);
        ui->save_state = NULL;
    }

    if (ui->primary_window) {
        ca_window_destroy(ui->primary_window);
        ui->primary_window = NULL;
    }

    if (ui->instance && ui->stylesheet) {
        ca_instance_set_stylesheet(ui->instance, NULL);
    }
    if (ui->stylesheet) {
        ca_css_destroy(ui->stylesheet);
        ui->stylesheet = NULL;
    }

    free(ui);
}

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui)
{
    return ui ? ui->primary_window : NULL;
}

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->save_state) {
        return false;
    }

    SolSaveState state;
    memset(&state, 0, sizeof(state));
    ca_state_get(ui->save_state, &state);
    ++state.count;
    ca_state_set(ui->save_state, &state);

    return true;
}

bool sol_ui_system_on_split_vertical_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, 0u, NULL)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_split_horizontal_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_HORIZONTAL, 0.5f, 0u, NULL)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_focus_next_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_focus_next_leaf(ui->buffers)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window)
{
    if (!ui || !window) {
        return;
    }

    if (ui->primary_window == window) {
        ui->primary_window = NULL;
        ui->workspace_host = NULL;
        ui->status_label = NULL;
    }
}
