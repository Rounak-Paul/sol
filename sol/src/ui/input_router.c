// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* input_router.c — Causality → Sol input plumbing.
 *
 * Mouse cache lives here so MOUSE_SCROLL can locate the pane under the
 * pointer without piping its position through global state. All seven
 * event hookups are owned by this module.
 */

#include "sol_input_router.h"

#include <causality.h>

#include <stdlib.h>
#include <string.h>

#include "sol_buffer.h"
#include "sol_input.h"
#include "sol_text_buffer.h"
#include "sol_text_view.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct SolInputRouter {
    Ca_Instance     *instance;
    SolUISystem     *ui;
    SolInputSystem  *input;
    SolBufferSystem *buffers;
    double           mouse_x;
    double           mouse_y;
    bool             suppress_next_text_input;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static SolModifierMask modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if (mods & 0x0001) out |= SOL_MOD_SHIFT;
    if (mods & 0x0002) out |= SOL_MOD_CTRL;
    if (mods & 0x0004) out |= SOL_MOD_ALT;
    if (mods & 0x0008) out |= SOL_MOD_SUPER;
    return out;
}

/* Printable keys arrive twice from GLFW — once as KEY and again decoded
   through CHAR. We only act on the CHAR copy so dead-keys / IME work. */
static bool key_is_printable_alpha(SolKeyCode key)
{
    return (key >= 32 && key <= 126);
}

/* Settle the cursor into view + ask the UI to rebuild the buffer area. */
static void post_edit_settle(SolInputRouter *r, SolTextBuffer *tb)
{
    if (!r || !tb) return;
    int win_h = 0;
    sol_ui_system_window_size(r->ui, NULL, &win_h);
    if (win_h <= 0) win_h = 600;
    const int viewport_full = sol_text_view_visible_lines(win_h);
    int viewport = viewport_full - 2;
    if (viewport < 1) viewport = 1;
    sol_text_buffer_ensure_cursor_visible(tb, viewport);
    sol_ui_system_invalidate_buffer_area(r->ui);
}

/* Handle non-printable / editing keys for the active text buffer. */
static bool handle_text_buffer_key(SolInputRouter *r,
                                   SolKeyCode key, SolModifierMask mods)
{
    SolTextBuffer *tb = sol_text_buffer_active(r->buffers);
    if (!tb) return false;

    /* Modifier-bearing chords (Cmd-S etc.) belong to the flow system;
       only consume the unadorned variants here. */
    const bool has_chord =
        (mods & (SOL_MOD_CTRL | SOL_MOD_SUPER | SOL_MOD_ALT)) != 0u;
    if (has_chord) return false;

    bool handled = false;
    switch (key) {
    case SOL_KEY_LEFT:      sol_text_buffer_move_cursor(tb, -1,  0, false); handled = true; break;
    case SOL_KEY_RIGHT:     sol_text_buffer_move_cursor(tb, +1,  0, false); handled = true; break;
    case SOL_KEY_UP:        sol_text_buffer_move_cursor(tb,  0, -1, true);  handled = true; break;
    case SOL_KEY_DOWN:      sol_text_buffer_move_cursor(tb,  0, +1, true);  handled = true; break;
    case SOL_KEY_HOME:      sol_text_buffer_move_line_start(tb); handled = true; break;
    case SOL_KEY_END:       sol_text_buffer_move_line_end(tb);   handled = true; break;
    case SOL_KEY_BACKSPACE: handled = sol_text_buffer_backspace(tb); break;
    case SOL_KEY_DELETE:    handled = sol_text_buffer_delete_forward(tb); break;
    case SOL_KEY_ENTER:     handled = sol_text_buffer_insert_newline(tb); break;
    default: break;
    }

    if (handled) post_edit_settle(r, tb);
    return handled;
}

/* ------------------------------------------------------------------ */
/* Causality event handlers                                            */
/* ------------------------------------------------------------------ */

static void on_key(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;

    SolInputEvent ie = {0};
    if (ev->key.action == CA_RELEASE) {
        ie.type = SOL_INPUT_EVENT_KEY_UP;
    } else if (ev->key.action == CA_PRESS || ev->key.action == CA_REPEAT) {
        ie.type = SOL_INPUT_EVENT_KEY_DOWN;
    } else {
        return;
    }
    ie.data.key.key       = (SolKeyCode)ev->key.key;
    ie.data.key.modifiers = modifiers_from_ca(ev->key.mods);
    ie.data.key.repeated  = (ev->key.action == CA_REPEAT);

    const bool ui_consumed = sol_ui_system_handle_input_event(r->ui, &ie);
    sol_input_system_process_event(r->input, &ie);

    /* Exact-match command flows can close the leader popup during
       KEY_DOWN; the corresponding CHAR event may still arrive after
       that and would otherwise be inserted into the active buffer.
       Latch a one-shot suppression for printable chord steps. */
    if (ui_consumed && ie.type == SOL_INPUT_EVENT_KEY_DOWN &&
        key_is_printable_alpha(ie.data.key.key)) {
        r->suppress_next_text_input = true;
    }

    if (ie.type != SOL_INPUT_EVENT_KEY_DOWN) return;
    if (ui_consumed) return;
    if (sol_ui_system_is_leader_active(r->ui)) return;
    if (key_is_printable_alpha(ie.data.key.key)) return;
    handle_text_buffer_key(r, ie.data.key.key, ie.data.key.modifiers);
}

static void on_char(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_TEXT_INPUT;
    ie.data.text.codepoint = ev->character.codepoint;
    sol_input_system_process_event(r->input, &ie);

    if (r->suppress_next_text_input) {
        r->suppress_next_text_input = false;
        return;
    }

    if (sol_ui_system_is_leader_active(r->ui)) return;
    const uint32_t cp = ev->character.codepoint;
    if (cp == 0u) return;
    if (cp < 0x20u && cp != 0x09u) return;  /* C0 controls except TAB */
    if (cp == 0x7Fu) return;                 /* DEL */

    SolTextBuffer *tb = sol_text_buffer_active(r->buffers);
    if (!tb) return;
    if (sol_text_buffer_insert_codepoint(tb, cp)) {
        post_edit_settle(r, tb);
    }
}

static void on_mouse_button(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;

    SolInputEvent ie = {0};
    if (ev->mouse_button.action == CA_RELEASE) {
        ie.type = SOL_INPUT_EVENT_MOUSE_UP;
    } else if (ev->mouse_button.action == CA_PRESS ||
               ev->mouse_button.action == CA_REPEAT) {
        ie.type = SOL_INPUT_EVENT_MOUSE_DOWN;
    } else {
        return;
    }
    ie.data.mouse_button.button    = (SolMouseButton)ev->mouse_button.button;
    ie.data.mouse_button.modifiers = modifiers_from_ca(ev->mouse_button.mods);
    ie.data.mouse_button.repeated  = (ev->mouse_button.action == CA_REPEAT);
    sol_input_system_process_event(r->input, &ie);
}

static void on_mouse_move(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;

    r->mouse_x = ev->mouse_pos.x;
    r->mouse_y = ev->mouse_pos.y;

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_MOUSE_MOVE;
    ie.data.mouse_move.x = ev->mouse_pos.x;
    ie.data.mouse_move.y = ev->mouse_pos.y;
    sol_input_system_process_event(r->input, &ie);
}

static void on_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_MOUSE_SCROLL;
    ie.data.mouse_scroll.x = (float)ev->mouse_scroll.dx;
    ie.data.mouse_scroll.y = (float)ev->mouse_scroll.dy;
    sol_input_system_process_event(r->input, &ie);

    if (!r->buffers || !r->ui) return;

    int win_w = 0, win_h = 0;
    sol_ui_system_window_size(r->ui, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    const float title_h  = (float)sol_ui_system_title_bar_height(r->ui);
    const float status_h = (float)sol_ui_system_status_bar_height(r->ui);
    const float tree_w   = (float)sol_ui_system_tree_panel_width(r->ui);
    const float buf_x = tree_w;
    const float buf_y = title_h;
    const float buf_w = (float)win_w - tree_w;
    const float buf_h = (float)win_h - title_h - status_h;

    SolBufferId target = 0u;
    const double mx = r->mouse_x, my = r->mouse_y;
    if (mx >= buf_x && mx <= buf_x + buf_w &&
        my >= buf_y && my <= buf_y + buf_h)
    {
        SolBufferNodeId leaf = sol_buffer_leaf_at_point(
            r->buffers, buf_x, buf_y, buf_w, buf_h,
            /* split-bar size — keep in sync with workspace.c */ 1.0f,
            (float)mx, (float)my);
        if (leaf != 0u) target = sol_buffer_leaf_buffer(r->buffers, leaf);
    }
    if (target == 0u) target = sol_buffer_active_buffer(r->buffers);
    if (target == 0u) return;

    SolBuffer *buf = sol_buffer_get(r->buffers, target);
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return;

    const int rendered = sol_text_view_visible_lines(win_h);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;
    const int total = (int)sol_text_buffer_line_count(tb);
    const int max_top = total > viewport ? total - viewport : 0;

    /* Natural scroll: dy>0 scrolls content up (view moves down). */
    int delta = (int)(-ev->mouse_scroll.dy * 3.0);
    if (delta == 0) {
        delta = ev->mouse_scroll.dy > 0.0 ? -1
              : ev->mouse_scroll.dy < 0.0 ? 1 : 0;
    }

    int new_top = sol_text_buffer_scroll_top(tb) + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;

    if (new_top != sol_text_buffer_scroll_top(tb)) {
        sol_text_buffer_set_scroll_top(tb, new_top);
        sol_ui_system_invalidate_buffer_area(r->ui);
    }
}

static void on_window_close(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    sol_ui_system_on_window_close(r->ui, ev->window);
}

static void on_window_resize(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    sol_ui_system_on_window_resize(r->ui, ev->resize.width, ev->resize.height);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolInputRouter *sol_input_router_create(Ca_Instance *instance, SolUISystem *ui,
                                        SolInputSystem *input,
                                        SolBufferSystem *buffers)
{
    if (!instance || !ui || !input || !buffers) return NULL;
    SolInputRouter *r = (SolInputRouter *)calloc(1u, sizeof(SolInputRouter));
    if (!r) return NULL;
    r->instance = instance;
    r->ui       = ui;
    r->input    = input;
    r->buffers  = buffers;

    ca_event_set_handler(instance, CA_EVENT_KEY,           on_key,           r);
    ca_event_set_handler(instance, CA_EVENT_CHAR,          on_char,          r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON,  on_mouse_button,  r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE,    on_mouse_move,    r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL,  on_mouse_scroll,  r);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE,  on_window_close,  r);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_window_resize, r);
    return r;
}

void sol_input_router_destroy(SolInputRouter *router)
{
    if (!router) return;
    if (router->instance) {
        ca_event_set_handler(router->instance, CA_EVENT_KEY,           NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_CHAR,          NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_MOUSE_BUTTON,  NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_MOUSE_MOVE,    NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_MOUSE_SCROLL,  NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_WINDOW_CLOSE,  NULL, NULL);
        ca_event_set_handler(router->instance, CA_EVENT_WINDOW_RESIZE, NULL, NULL);
    }
    free(router);
}
