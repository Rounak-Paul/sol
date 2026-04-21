#include "sol_input.h"

#include "sol_threading.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOL_INPUT_MAX_KEYS 512u
#define SOL_INPUT_MAX_MOUSE_BUTTONS 16u

typedef struct SolInputBinding {
    SolInputActionToken token;
    char *action;
    SolKeyCode key;
    SolModifierMask required_modifiers;
    SolModifierMask forbidden_modifiers;
    bool trigger_on_release;
    bool allow_repeat;
    int priority;
    SolInputActionCallback callback;
    void *user_data;
    bool active;
} SolInputBinding;

typedef struct SolInputActionEvent {
    const char *action;
    SolInputEvent source_event;
} SolInputActionEvent;

struct SolInputSystem {
    pthread_mutex_t lock;

    bool key_down[SOL_INPUT_MAX_KEYS];
    bool key_pressed[SOL_INPUT_MAX_KEYS];
    bool key_released[SOL_INPUT_MAX_KEYS];

    bool mouse_down[SOL_INPUT_MAX_MOUSE_BUTTONS];
    bool mouse_pressed[SOL_INPUT_MAX_MOUSE_BUTTONS];
    bool mouse_released[SOL_INPUT_MAX_MOUSE_BUTTONS];

    double mouse_x;
    double mouse_y;
    float scroll_x;
    float scroll_y;

    SolModifierMask modifiers;

    SolInputBinding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    SolInputActionToken next_token;

    SolEventBus *event_bus;
};

static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *out = (char *)malloc(len + 1u);
    if (!out) {
        return NULL;
    }

    memcpy(out, value, len + 1u);
    return out;
}

static bool sol_input_reserve_bindings(SolInputSystem *system, size_t min_capacity)
{
    if (system->binding_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = system->binding_capacity == 0u ? 16u : system->binding_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolInputBinding *new_items = (SolInputBinding *)realloc(
        system->bindings,
        new_capacity * sizeof(SolInputBinding)
    );
    if (!new_items) {
        return false;
    }

    system->bindings = new_items;
    system->binding_capacity = new_capacity;
    return true;
}

static void sol_input_compact_bindings_locked(SolInputSystem *system)
{
    size_t write = 0u;
    for (size_t read = 0u; read < system->binding_count; ++read) {
        if (!system->bindings[read].active) {
            free(system->bindings[read].action);
            continue;
        }

        if (write != read) {
            system->bindings[write] = system->bindings[read];
        }

        ++write;
    }

    system->binding_count = write;
}

static const char *sol_input_event_name(SolInputEventType type)
{
    switch (type) {
        case SOL_INPUT_EVENT_KEY_DOWN:
            return "input.key.down";
        case SOL_INPUT_EVENT_KEY_UP:
            return "input.key.up";
        case SOL_INPUT_EVENT_TEXT_INPUT:
            return "input.text";
        case SOL_INPUT_EVENT_MOUSE_MOVE:
            return "input.mouse.move";
        case SOL_INPUT_EVENT_MOUSE_DOWN:
            return "input.mouse.down";
        case SOL_INPUT_EVENT_MOUSE_UP:
            return "input.mouse.up";
        case SOL_INPUT_EVENT_MOUSE_SCROLL:
            return "input.mouse.scroll";
        default:
            return "input.unknown";
    }
}

static SolModifierMask sol_modifier_for_key(SolKeyCode key)
{
    switch (key) {
        case SOL_KEY_LEFT_SHIFT:
        case SOL_KEY_RIGHT_SHIFT:
            return SOL_MOD_SHIFT;
        case SOL_KEY_LEFT_CTRL:
        case SOL_KEY_RIGHT_CTRL:
            return SOL_MOD_CTRL;
        case SOL_KEY_LEFT_ALT:
        case SOL_KEY_RIGHT_ALT:
            return SOL_MOD_ALT;
        case SOL_KEY_LEFT_SUPER:
        case SOL_KEY_RIGHT_SUPER:
            return SOL_MOD_SUPER;
        default:
            return SOL_MOD_NONE;
    }
}

SolInputConfig sol_input_config_default(void)
{
    SolInputConfig config;
    config.initial_binding_capacity = 32u;
    config.event_bus = NULL;
    return config;
}

SolInputSystem *sol_input_system_create(const SolInputConfig *config)
{
    SolInputConfig effective = config ? *config : sol_input_config_default();
    if (effective.initial_binding_capacity == 0u) {
        effective.initial_binding_capacity = 32u;
    }

    SolInputSystem *system = (SolInputSystem *)calloc(1u, sizeof(SolInputSystem));
    if (!system) {
        return NULL;
    }

    if (pthread_mutex_init(&system->lock, NULL) != 0) {
        free(system);
        return NULL;
    }

    system->bindings = (SolInputBinding *)calloc(effective.initial_binding_capacity, sizeof(SolInputBinding));
    if (!system->bindings) {
        pthread_mutex_destroy(&system->lock);
        free(system);
        return NULL;
    }

    system->binding_capacity = effective.initial_binding_capacity;
    system->next_token = 1u;
    system->event_bus = effective.event_bus;
    return system;
}

void sol_input_system_destroy(SolInputSystem *system)
{
    if (!system) {
        return;
    }

    for (size_t i = 0u; i < system->binding_count; ++i) {
        free(system->bindings[i].action);
    }

    free(system->bindings);
    pthread_mutex_destroy(&system->lock);
    free(system);
}

void sol_input_system_begin_frame(SolInputSystem *system)
{
    if (!system) {
        return;
    }

    pthread_mutex_lock(&system->lock);
    memset(system->key_pressed, 0, sizeof(system->key_pressed));
    memset(system->key_released, 0, sizeof(system->key_released));
    memset(system->mouse_pressed, 0, sizeof(system->mouse_pressed));
    memset(system->mouse_released, 0, sizeof(system->mouse_released));
    system->scroll_x = 0.0f;
    system->scroll_y = 0.0f;
    pthread_mutex_unlock(&system->lock);
}

static bool sol_input_event_matches_binding(
    const SolInputBinding *binding,
    const SolInputEvent *event,
    SolModifierMask current_modifiers
)
{
    if (!binding || !event) {
        return false;
    }

    bool release_event = false;
    bool repeated_event = false;
    SolKeyCode event_key = SOL_KEY_UNKNOWN;

    if (event->type == SOL_INPUT_EVENT_KEY_DOWN) {
        event_key = event->data.key.key;
        repeated_event = event->data.key.repeated;
        release_event = false;
    } else if (event->type == SOL_INPUT_EVENT_KEY_UP) {
        event_key = event->data.key.key;
        repeated_event = false;
        release_event = true;
    } else {
        return false;
    }

    if (event_key != binding->key) {
        return false;
    }
    if (release_event != binding->trigger_on_release) {
        return false;
    }
    if (repeated_event && !binding->allow_repeat) {
        return false;
    }
    if ((current_modifiers & binding->required_modifiers) != binding->required_modifiers) {
        return false;
    }
    if ((current_modifiers & binding->forbidden_modifiers) != 0u) {
        return false;
    }

    return true;
}

bool sol_input_system_process_event(SolInputSystem *system, const SolInputEvent *event)
{
    if (!system || !event) {
        return false;
    }

    SolInputBinding *matches = NULL;
    size_t match_count = 0u;
    size_t match_capacity = 0u;
    SolModifierMask modifiers = SOL_MOD_NONE;

    pthread_mutex_lock(&system->lock);

    if (event->type == SOL_INPUT_EVENT_KEY_DOWN || event->type == SOL_INPUT_EVENT_KEY_UP) {
        const SolKeyCode key = event->data.key.key;
        if (key < SOL_INPUT_MAX_KEYS) {
            if (event->type == SOL_INPUT_EVENT_KEY_DOWN) {
                const bool was_down = system->key_down[key];
                system->key_down[key] = true;
                if (!was_down) {
                    system->key_pressed[key] = true;
                }
            } else {
                system->key_down[key] = false;
                system->key_released[key] = true;
            }
        }

        const SolModifierMask bit = sol_modifier_for_key(key);
        if (bit != SOL_MOD_NONE) {
            if (event->type == SOL_INPUT_EVENT_KEY_DOWN) {
                system->modifiers |= bit;
            } else {
                system->modifiers &= (SolModifierMask)(~bit);
            }
        }
    } else if (event->type == SOL_INPUT_EVENT_MOUSE_MOVE) {
        system->mouse_x = event->data.mouse_move.x;
        system->mouse_y = event->data.mouse_move.y;
    } else if (event->type == SOL_INPUT_EVENT_MOUSE_DOWN || event->type == SOL_INPUT_EVENT_MOUSE_UP) {
        const SolMouseButton button = event->data.mouse_button.button;
        if (button < SOL_INPUT_MAX_MOUSE_BUTTONS) {
            if (event->type == SOL_INPUT_EVENT_MOUSE_DOWN) {
                const bool was_down = system->mouse_down[button];
                system->mouse_down[button] = true;
                if (!was_down) {
                    system->mouse_pressed[button] = true;
                }
            } else {
                system->mouse_down[button] = false;
                system->mouse_released[button] = true;
            }
        }
    } else if (event->type == SOL_INPUT_EVENT_MOUSE_SCROLL) {
        system->scroll_x += event->data.mouse_scroll.x;
        system->scroll_y += event->data.mouse_scroll.y;
    }

    modifiers = system->modifiers;

    if (event->type == SOL_INPUT_EVENT_KEY_DOWN || event->type == SOL_INPUT_EVENT_KEY_UP) {
        for (size_t i = 0u; i < system->binding_count; ++i) {
            const SolInputBinding *binding = &system->bindings[i];
            if (!binding->active) {
                continue;
            }
            if (!sol_input_event_matches_binding(binding, event, modifiers)) {
                continue;
            }

            if (match_count == match_capacity) {
                size_t new_capacity = match_capacity == 0u ? 8u : (match_capacity * 2u);
                SolInputBinding *new_matches = (SolInputBinding *)realloc(
                    matches,
                    new_capacity * sizeof(SolInputBinding)
                );
                if (!new_matches) {
                    free(matches);
                    pthread_mutex_unlock(&system->lock);
                    return false;
                }

                matches = new_matches;
                match_capacity = new_capacity;
            }

            matches[match_count++] = *binding;
        }
    }

    pthread_mutex_unlock(&system->lock);

    bool consumed = false;

    if (system->event_bus) {
        SolEventDesc bus_event;
        bus_event.event_type = 0u;
        bus_event.event_name = sol_input_event_name(event->type);
        bus_event.payload = event;
        bus_event.payload_size = sizeof(*event);
        bus_event.sender = system;
        bus_event.flags = SOL_EVENT_FLAG_NONE;
        sol_event_bus_publish(system->event_bus, &bus_event);
    }

    for (size_t i = 0u; i < match_count; ++i) {
        if (matches[i].callback) {
            const bool handled = matches[i].callback(matches[i].action, event, matches[i].user_data);
            if (handled) {
                consumed = true;
            }
        }

        if (system->event_bus) {
            SolInputActionEvent action_event;
            action_event.action = matches[i].action;
            action_event.source_event = *event;

            SolEventDesc bus_event;
            bus_event.event_type = 0u;
            bus_event.event_name = "input.action";
            bus_event.payload = &action_event;
            bus_event.payload_size = sizeof(action_event);
            bus_event.sender = system;
            bus_event.flags = SOL_EVENT_FLAG_NONE;
            sol_event_bus_publish(system->event_bus, &bus_event);
        }

        if (consumed) {
            break;
        }
    }

    free(matches);
    return consumed;
}

SolInputActionToken sol_input_bind_action(
    SolInputSystem *system,
    const SolInputBindingDesc *desc
)
{
    if (!system || !desc || !desc->action || desc->key == SOL_KEY_UNKNOWN) {
        return 0u;
    }

    SolInputBinding binding;
    memset(&binding, 0, sizeof(binding));

    binding.action = sol_strdup(desc->action);
    if (!binding.action) {
        return 0u;
    }

    binding.key = desc->key;
    binding.required_modifiers = desc->required_modifiers;
    binding.forbidden_modifiers = desc->forbidden_modifiers;
    binding.trigger_on_release = desc->trigger_on_release;
    binding.allow_repeat = desc->allow_repeat;
    binding.priority = desc->priority;
    binding.callback = desc->callback;
    binding.user_data = desc->user_data;
    binding.active = true;

    pthread_mutex_lock(&system->lock);

    if (!sol_input_reserve_bindings(system, system->binding_count + 1u)) {
        pthread_mutex_unlock(&system->lock);
        free(binding.action);
        return 0u;
    }

    binding.token = system->next_token++;

    size_t insert_at = system->binding_count;
    for (size_t i = 0u; i < system->binding_count; ++i) {
        if (binding.priority > system->bindings[i].priority) {
            insert_at = i;
            break;
        }
    }

    for (size_t i = system->binding_count; i > insert_at; --i) {
        system->bindings[i] = system->bindings[i - 1u];
    }

    system->bindings[insert_at] = binding;
    ++system->binding_count;

    pthread_mutex_unlock(&system->lock);
    return binding.token;
}

bool sol_input_unbind_action(SolInputSystem *system, SolInputActionToken token)
{
    if (!system || token == 0u) {
        return false;
    }

    bool removed = false;

    pthread_mutex_lock(&system->lock);
    for (size_t i = 0u; i < system->binding_count; ++i) {
        if (system->bindings[i].token != token) {
            continue;
        }

        system->bindings[i].active = false;
        removed = true;
        break;
    }

    if (removed) {
        sol_input_compact_bindings_locked(system);
    }

    pthread_mutex_unlock(&system->lock);
    return removed;
}

bool sol_input_is_key_down(SolInputSystem *system, SolKeyCode key)
{
    if (!system || key >= SOL_INPUT_MAX_KEYS) {
        return false;
    }

    pthread_mutex_lock(&system->lock);
    const bool value = system->key_down[key];
    pthread_mutex_unlock(&system->lock);
    return value;
}

bool sol_input_was_key_pressed(SolInputSystem *system, SolKeyCode key)
{
    if (!system || key >= SOL_INPUT_MAX_KEYS) {
        return false;
    }

    pthread_mutex_lock(&system->lock);
    const bool value = system->key_pressed[key];
    pthread_mutex_unlock(&system->lock);
    return value;
}

bool sol_input_was_key_released(SolInputSystem *system, SolKeyCode key)
{
    if (!system || key >= SOL_INPUT_MAX_KEYS) {
        return false;
    }

    pthread_mutex_lock(&system->lock);
    const bool value = system->key_released[key];
    pthread_mutex_unlock(&system->lock);
    return value;
}

SolModifierMask sol_input_current_modifiers(SolInputSystem *system)
{
    if (!system) {
        return SOL_MOD_NONE;
    }

    pthread_mutex_lock(&system->lock);
    const SolModifierMask modifiers = system->modifiers;
    pthread_mutex_unlock(&system->lock);
    return modifiers;
}

void sol_input_mouse_position(SolInputSystem *system, double *out_x, double *out_y)
{
    if (!system) {
        if (out_x) {
            *out_x = 0.0;
        }
        if (out_y) {
            *out_y = 0.0;
        }
        return;
    }

    pthread_mutex_lock(&system->lock);
    const double x = system->mouse_x;
    const double y = system->mouse_y;
    pthread_mutex_unlock(&system->lock);

    if (out_x) {
        *out_x = x;
    }
    if (out_y) {
        *out_y = y;
    }
}

void sol_input_consume_scroll(SolInputSystem *system, float *out_x, float *out_y)
{
    if (!system) {
        if (out_x) {
            *out_x = 0.0f;
        }
        if (out_y) {
            *out_y = 0.0f;
        }
        return;
    }

    pthread_mutex_lock(&system->lock);
    const float x = system->scroll_x;
    const float y = system->scroll_y;
    system->scroll_x = 0.0f;
    system->scroll_y = 0.0f;
    pthread_mutex_unlock(&system->lock);

    if (out_x) {
        *out_x = x;
    }
    if (out_y) {
        *out_y = y;
    }
}
