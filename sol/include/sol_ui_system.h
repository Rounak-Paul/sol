#ifndef SOL_UI_SYSTEM_H
#define SOL_UI_SYSTEM_H

#include <stdbool.h>

#include <causality.h>

#include "sol_buffer.h"
#include "sol_input.h"

typedef struct SolUISystem SolUISystem;

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers);
void sol_ui_system_destroy(SolUISystem *ui);

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui);

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_split_vertical_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_split_horizontal_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_focus_next_action(const char *action, const SolInputEvent *event, void *user_data);

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window);

#endif
