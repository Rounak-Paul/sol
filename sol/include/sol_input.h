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

/* Discriminates the active union member in a SolInputEvent. */
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

/* A single platform input event delivered to the input system. */
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

/*
 * Callback fired when a bound action is triggered.
 *
 * action     The action string from the binding descriptor.
 * event      The raw input event that triggered the binding.
 * user_data  Caller-supplied pointer from the binding descriptor.
 * Returns    true to indicate the event was consumed.
 */
typedef bool (*SolInputActionCallback)(
    const char *action,
    const SolInputEvent *event,
    void *user_data
);

/* Parameters for registering a single key action binding. */
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

/* Initialization parameters for the input system. */
typedef struct SolInputConfig {
    size_t initial_binding_capacity;
    SolEventBus *event_bus;
} SolInputConfig;

/* Return a SolInputConfig populated with sensible defaults. */
SolInputConfig sol_input_config_default(void);

/*
 * Create a new input system.
 *
 * config  Capacity and optional event bus; use sol_input_config_default().
 * Returns A heap-allocated system, or NULL on allocation failure.
 */
SolInputSystem *sol_input_system_create(const SolInputConfig *config);

/* Destroy the input system and free all internal resources. */
void sol_input_system_destroy(SolInputSystem *system);

/* Reset per-frame transient state (pressed/released sets). Call once per frame. */
void sol_input_system_begin_frame(SolInputSystem *system);

/*
 * Deliver a single platform event to the input system.
 *
 * system  The input system.
 * event   The event to process.
 * Returns true if the event triggered a bound action.
 */
bool sol_input_system_process_event(SolInputSystem *system, const SolInputEvent *event);

/*
 * Register a key action binding.
 *
 * system  The input system.
 * desc    Binding parameters (key, modifiers, callback, etc.).
 * Returns An opaque token used to unbind later; 0 on failure.
 */
SolInputActionToken sol_input_bind_action(
    SolInputSystem *system,
    const SolInputBindingDesc *desc
);

/*
 * Remove a previously registered binding.
 *
 * system  The input system.
 * token   Token returned by sol_input_bind_action.
 * Returns true if the binding was found and removed.
 */
bool sol_input_unbind_action(SolInputSystem *system, SolInputActionToken token);

/* Returns true if the given key is currently held down. */
bool sol_input_is_key_down(SolInputSystem *system, SolKeyCode key);

/* Returns true if the given key transitioned to pressed this frame. */
bool sol_input_was_key_pressed(SolInputSystem *system, SolKeyCode key);

/* Returns true if the given key transitioned to released this frame. */
bool sol_input_was_key_released(SolInputSystem *system, SolKeyCode key);

/* Returns the bitmask of modifier keys currently held. */
SolModifierMask sol_input_current_modifiers(SolInputSystem *system);

/*
 * Write the last-known mouse cursor position into out_x and out_y.
 *
 * system  The input system.
 * out_x   Receives the x coordinate (may be NULL).
 * out_y   Receives the y coordinate (may be NULL).
 */
void sol_input_mouse_position(SolInputSystem *system, double *out_x, double *out_y);

/*
 * Read and clear the accumulated scroll delta for this frame.
 *
 * system  The input system.
 * out_x   Receives the horizontal scroll delta (may be NULL).
 * out_y   Receives the vertical scroll delta (may be NULL).
 */
void sol_input_consume_scroll(SolInputSystem *system, float *out_x, float *out_y);

#endif
