#include "sol_ui_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "style.h"

typedef struct SolSaveState {
    uint32_t count;
} SolSaveState;

struct SolUISystem {
    Ca_Instance *instance;
    Ca_Window *main_window;
    Ca_State *save_state;
    Ca_Label *save_label;
};

static void sol_ui_on_save_state_changed(const void *value, void *user_data)
{
    if (!value || !user_data) {
        return;
    }

    const SolSaveState *state = (const SolSaveState *)value;
    SolUISystem *ui = (SolUISystem *)user_data;

    if (!ui->save_label) {
        return;
    }

    char line[64];
    snprintf(line, sizeof(line), "Saves: %u", state->count);
    ca_set_text(ui->save_label, line);
}

static bool sol_ui_build_main_window(SolUISystem *ui)
{
    if (!ui || !ui->main_window) {
        return false;
    }

    ca_ui_begin(ui->main_window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "app-root",
    });

    ca_h1(&(Ca_TextDesc){
        .text = "Hello Sol",
        .style = "title",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Press Ctrl+S to trigger editor.save",
        .style = "subtitle",
    });

    ui->save_label = ca_text(&(Ca_TextDesc){
        .text = "Saves: 0",
        .style = "status-line",
    });

    ca_ui_end();
    return true;
}

SolUISystem *sol_ui_system_create(Ca_Instance *instance)
{
    if (!instance) {
        return NULL;
    }

    SolUISystem *ui = (SolUISystem *)calloc(1u, sizeof(SolUISystem));
    if (!ui) {
        return NULL;
    }

    ui->instance = instance;

    Ca_Stylesheet *sheet = ca_css_parse(SOL_UI_MAIN_WINDOW_CSS);
    if (sheet) {
        ca_instance_set_stylesheet(instance, sheet);
    }

    ui->main_window = ca_window_create(instance, &(Ca_WindowDesc){
        .title = "Sol",
        .width = 800,
        .height = 600,
    });

    if (!ui->main_window) {
        free(ui);
        return NULL;
    }

    SolSaveState initial = { 0u };
    ui->save_state = ca_state_create(instance, sizeof(SolSaveState), &initial);
    if (ui->save_state) {
        ca_state_observe(ui->save_state, sol_ui_on_save_state_changed, ui);
    }

    if (!sol_ui_build_main_window(ui)) {
        ca_window_destroy(ui->main_window);
        if (ui->save_state) {
            ca_state_destroy(ui->save_state);
        }
        free(ui);
        return NULL;
    }

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

    if (ui->main_window) {
        ca_window_destroy(ui->main_window);
        ui->main_window = NULL;
    }

    free(ui);
}

Ca_Window *sol_ui_system_main_window(SolUISystem *ui)
{
    return ui ? ui->main_window : NULL;
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

    printf("[sol] action: editor.save (%u)\n", state.count);
    return true;
}

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window)
{
    if (!ui || !window) {
        return;
    }

    if (ui->main_window == window) {
        ui->main_window = NULL;
        ui->save_label = NULL;
    }
}
