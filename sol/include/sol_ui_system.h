#ifndef SOL_UI_SYSTEM_H
#define SOL_UI_SYSTEM_H

#include <stdbool.h>

#include <causality.h>

#include "sol_input.h"

typedef struct SolUISystem SolUISystem;

SolUISystem *sol_ui_system_create(Ca_Instance *instance);
void sol_ui_system_destroy(SolUISystem *ui);

Ca_Window *sol_ui_system_main_window(SolUISystem *ui);

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data);

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window);

#endif
