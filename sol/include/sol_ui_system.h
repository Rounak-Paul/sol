#ifndef SOL_UI_SYSTEM_H
#define SOL_UI_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>

#include <causality.h>

#include "sol_buffer.h"
#include "sol_input.h"

typedef struct SolUISystem SolUISystem;

typedef struct SolCommandFlowDesc {
	const char *action;
	const char *label;
	const SolKeyCode *sequence;
	size_t sequence_length;
	SolKeyCode key;
	SolInputActionCallback callback;
	void *user_data;
} SolCommandFlowDesc;

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers);
void sol_ui_system_destroy(SolUISystem *ui);

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui);

bool sol_ui_system_register_command_flow(SolUISystem *ui, const SolCommandFlowDesc *desc);
bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event);

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_split_vertical_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_split_horizontal_action(const char *action, const SolInputEvent *event, void *user_data);
bool sol_ui_system_on_focus_next_action(const char *action, const SolInputEvent *event, void *user_data);

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window);
void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height);

#endif
