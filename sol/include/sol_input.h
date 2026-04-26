// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_input.h — minimal input glue between causality events and sol's
// modal/editor layer. Just typed event translation; no event bus, no
// priority queue, no flow registration. The modal layer dispatches.

#ifndef SOL_INPUT_H
#define SOL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t SolKeyCode;
typedef uint8_t  SolModifierMask;

enum {
    SOL_MOD_NONE  = 0,
    SOL_MOD_SHIFT = 1u << 0,
    SOL_MOD_CTRL  = 1u << 1,
    SOL_MOD_ALT   = 1u << 2,
    SOL_MOD_SUPER = 1u << 3,
};

/* Mirrors GLFW key codes for the keys the editor cares about. */
enum {
    SOL_KEY_UNKNOWN     = 0,
    SOL_KEY_BACKSPACE   = 259,
    SOL_KEY_TAB         = 258,
    SOL_KEY_ENTER       = 257,
    SOL_KEY_ESCAPE      = 256,
    SOL_KEY_LEFT        = 263,
    SOL_KEY_RIGHT       = 262,
    SOL_KEY_UP          = 265,
    SOL_KEY_DOWN        = 264,
    SOL_KEY_HOME        = 268,
    SOL_KEY_END         = 269,
    SOL_KEY_PAGE_UP     = 266,
    SOL_KEY_PAGE_DOWN   = 267,
    SOL_KEY_DELETE      = 261,
    SOL_KEY_INSERT      = 260,
};

typedef enum SolInputKind {
    SOL_INPUT_KEY_DOWN,
    SOL_INPUT_KEY_UP,
    SOL_INPUT_TEXT,
} SolInputKind;

typedef struct SolInputEvent {
    SolInputKind    kind;
    SolKeyCode      key;        /* KEY_*  */
    SolModifierMask mods;       /* KEY_*  */
    bool            repeat;     /* KEY_DOWN */
    uint32_t        codepoint;  /* TEXT   */
} SolInputEvent;

/* Translate raw GLFW-style modifier bits coming from a Ca_Event. */
SolModifierMask sol_modifiers_from_ca(int mods);

#ifdef __cplusplus
}
#endif

#endif
