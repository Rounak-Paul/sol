#ifndef SOL_INPUT_H
#define SOL_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_event.h"

typedef struct SolInputSystem SolInputSystem;
typedef uint64_t SolInputActionToken;

typedef uint32_t SolKeyCode;
typedef uint32_t SolMouseButton;
typedef uint8_t SolModifierMask;

enum {
    SOL_MOD_NONE = 0,
    SOL_MOD_SHIFT = 1u << 0,
    SOL_MOD_CTRL = 1u << 1,
    SOL_MOD_ALT = 1u << 2,
    SOL_MOD_SUPER = 1u << 3,
};

enum {
    SOL_KEY_UNKNOWN = 0,
    /* GLFW-aligned non-printable keys. We pass GLFW key codes through
       unchanged in main.c, so these constants are just named aliases. */
    SOL_KEY_ENTER     = 257,
    SOL_KEY_TAB       = 258,
    SOL_KEY_BACKSPACE = 259,
    SOL_KEY_INSERT    = 260,
    SOL_KEY_DELETE    = 261,
    SOL_KEY_RIGHT     = 262,
    SOL_KEY_LEFT      = 263,
    SOL_KEY_DOWN      = 264,
    SOL_KEY_UP        = 265,
    SOL_KEY_PAGE_UP   = 266,
    SOL_KEY_PAGE_DOWN = 267,
    SOL_KEY_HOME      = 268,
    SOL_KEY_END       = 269,
    SOL_KEY_ESCAPE = 256,
    SOL_KEY_LEFT_SHIFT = 340,
    SOL_KEY_LEFT_CTRL = 341,
    SOL_KEY_LEFT_ALT = 342,
    SOL_KEY_LEFT_SUPER = 343,
    SOL_KEY_RIGHT_SHIFT = 344,
    SOL_KEY_RIGHT_CTRL = 345,
    SOL_KEY_RIGHT_ALT = 346,
    SOL_KEY_RIGHT_SUPER = 347,
};

typedef enum SolInputEventType {
    SOL_INPUT_EVENT_NONE = 0,
    SOL_INPUT_EVENT_KEY_DOWN,
    SOL_INPUT_EVENT_KEY_UP,
    SOL_INPUT_EVENT_TEXT_INPUT,
    SOL_INPUT_EVENT_MOUSE_MOVE,
    SOL_INPUT_EVENT_MOUSE_DOWN,
    SOL_INPUT_EVENT_MOUSE_UP,
    SOL_INPUT_EVENT_MOUSE_SCROLL,
} SolInputEventType;

typedef struct SolInputEvent {
    SolInputEventType type;
    uint64_t timestamp_ns;
    union {
        struct {
            SolKeyCode key;
            SolModifierMask modifiers;
            bool repeated;
        } key;
        struct {
            uint32_t codepoint;
        } text;
        struct {
            double x;
            double y;
            double delta_x;
            double delta_y;
        } mouse_move;
        struct {
            SolMouseButton button;
            SolModifierMask modifiers;
            bool repeated;
        } mouse_button;
        struct {
            float x;
            float y;
        } mouse_scroll;
    } data;
} SolInputEvent;

typedef bool (*SolInputActionCallback)(
    const char *action,
    const SolInputEvent *event,
    void *user_data
);

typedef struct SolInputBindingDesc {
    const char *action;
    SolKeyCode key;
    SolModifierMask required_modifiers;
    SolModifierMask forbidden_modifiers;
    bool trigger_on_release;
    bool allow_repeat;
    int priority;
    SolInputActionCallback callback;
    void *user_data;
} SolInputBindingDesc;

typedef struct SolInputConfig {
    size_t initial_binding_capacity;
    SolEventBus *event_bus;
} SolInputConfig;

SolInputConfig sol_input_config_default(void);

SolInputSystem *sol_input_system_create(const SolInputConfig *config);
void sol_input_system_destroy(SolInputSystem *system);

void sol_input_system_begin_frame(SolInputSystem *system);
bool sol_input_system_process_event(SolInputSystem *system, const SolInputEvent *event);

SolInputActionToken sol_input_bind_action(
    SolInputSystem *system,
    const SolInputBindingDesc *desc
);

bool sol_input_unbind_action(SolInputSystem *system, SolInputActionToken token);

bool sol_input_is_key_down(SolInputSystem *system, SolKeyCode key);
bool sol_input_was_key_pressed(SolInputSystem *system, SolKeyCode key);
bool sol_input_was_key_released(SolInputSystem *system, SolKeyCode key);

SolModifierMask sol_input_current_modifiers(SolInputSystem *system);
void sol_input_mouse_position(SolInputSystem *system, double *out_x, double *out_y);
void sol_input_consume_scroll(SolInputSystem *system, float *out_x, float *out_y);

#endif
