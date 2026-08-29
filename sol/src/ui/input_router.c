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
#include "sol_terminal.h"
#include "sol_text_buffer.h"
#include "sol_text_view.h"
#include "sol_ui_constants.h"
#include "sol_ui_internal.h"
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
    double           horizontal_scroll_remainder;
    bool             buffer_input_active;
    bool             suppress_next_text_input;
    bool             terminal_mouse_down;  /* button held while reporting to a mouse-aware TUI */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Convert a Causality modifier bitmask to the Sol modifier mask type.
 *
 * mods    Causality modifier flags (bit 0=Shift, 1=Ctrl, 2=Alt, 3=Super).
 * Returns Equivalent SolModifierMask.
 */
static SolModifierMask modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if (mods & 0x0001) out |= SOL_MOD_SHIFT;
    if (mods & 0x0002) out |= SOL_MOD_CTRL;
    if (mods & 0x0004) out |= SOL_MOD_ALT;
    if (mods & 0x0008) out |= SOL_MOD_SUPER;
    return out;
}

/* Resolve the monospace advance used by the text view, in layout pixels. */
/*
 * Return the monospace glyph advance width in layout pixels, measured from
 * the window's font metrics.  Falls back to 60% of the boot font size when
 * the measurement is unavailable.
 *
 * win     The Causality window to query (may be NULL).
 * Returns Glyph advance width in logical pixels.
 */
static float router_glyph_advance_px(Ca_Window *win)
{
    float w = win ? ca_measure_text_px(win, "M", SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)
                  : 0.0f;
    if (w <= 0.0f) w = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;
    return w;
}

/* Convert one wheel axis to editor scroll columns/rows. */
/*
 * Convert a raw wheel-axis amount to an integer scroll delta in columns/rows.
 * Guarantees at least ±1 when amount is non-zero.
 *
 * amount  Raw wheel axis value.
 * Returns Integer scroll delta.
 */
static int scroll_delta_from_axis(double amount)
{
    int delta = (int)(amount * 3.0);
    if (delta == 0) {
        delta = amount > 0.0 ? 1 : amount < 0.0 ? -1 : 0;
    }
    return delta;
}

/*
 * Accumulate a fractional horizontal scroll amount and return the integer
 * column delta, carrying the sub-column remainder into the next call.
 *
 * r   The input router holding the fractional remainder.
 * dx  Raw horizontal wheel axis value.
 * Returns Integer column delta (positive = scroll right).
 */
static int horizontal_scroll_delta(SolInputRouter *r, double dx)
{
    if (!r || dx == 0.0) return 0;
    r->horizontal_scroll_remainder += -dx * 3.0;
    const int delta = (int)r->horizontal_scroll_remainder;
    if (delta != 0) {
        r->horizontal_scroll_remainder -= (double)delta;
    }
    return delta;
}

/*
 * Return true when the event originated from the UI system's primary window.
 *
 * r   The input router providing the UI system reference.
 * ev  The Causality event to check.
 */
static bool event_is_from_primary_window(const SolInputRouter *r,
                                         const Ca_Event *ev)
{
    if (!r || !r->ui || !ev || !ev->window) return false;
    return ev->window == sol_ui_system_primary_window(r->ui);
}

/*
 * Test whether the screen point (x, y) falls inside the active buffer area
 * and, if so, resolve the leaf buffer-split node it belongs to.
 *
 * r         The input router providing UI and buffer system references.
 * x         Screen X coordinate to test.
 * y         Screen Y coordinate to test.
 * out_root  Optionally receives the buffer-area bounding rect.
 * out_leaf  Optionally receives the leaf node ID (0 when not inside any pane).
 * Returns   true when the point is inside a valid buffer pane.
 */
static bool point_in_active_buffer_leaf(SolInputRouter *r,
                                        double x, double y,
                                        SolBufferRect *out_root,
                                        SolBufferNodeId *out_leaf)
{
    if (out_leaf) *out_leaf = 0u;
    if (!r || !r->ui || !r->buffers) return false;

    SolBufferRect root = {0};
    if (!sol_ui_system_buffer_area_rect(r->ui, &root.x, &root.y,
                                        &root.w, &root.h)) {
        return false;
    }
    if (out_root) *out_root = root;

    if (x < root.x || x > root.x + root.w ||
        y < root.y || y > root.y + root.h) {
        return false;
    }

    const SolBufferNodeId leaf = sol_buffer_leaf_at_point(
        r->buffers, root.x, root.y, root.w, root.h,
        SOL_UI_PANEL_GAP_PX * sol_ui_system_scale(r->ui),
        (float)x, (float)y);
    if (leaf == 0u) return false;
    if (out_leaf) *out_leaf = leaf;
    return true;
}

/*
 * Resolve a window-space point to a terminal grid cell, when the terminal
 * panel is visible and the point falls inside its viewport (excluding the
 * tab header strip).  Mirrors the rect math sol_ui_system_pre_tick uses to
 * size the grid, so hit-testing always agrees with what is actually drawn.
 *
 * r          The input router.
 * x, y       Window-space point to test.
 * out_col    Set to the 0-based column when the point hits the grid.
 * out_row    Set to the 0-based row when the point hits the grid.
 * Returns    true if the point is inside the terminal viewport's cell grid.
 */
static bool terminal_cell_at_point(SolInputRouter *r, double x, double y,
                                   int *out_col, int *out_row)
{
    if (!r || !r->ui) return false;
    SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
    if (!tmgr || !sol_terminal_manager_visible(tmgr)) return false;

    float bx, by, bw, bh;
    if (!sol_ui_system_buffer_area_rect(r->ui, &bx, &by, &bw, &bh) ||
        bw <= 0.0f || bh <= 0.0f) {
        return false;
    }

    const float ratio = sol_terminal_manager_ratio(tmgr);
    const SolTerminalPosition pos = sol_terminal_manager_position(tmgr);
    float vx, vy, vw, vh; /* terminal panel rect, header not yet excluded */
    if (pos == SOL_TERMINAL_POSITION_BOTTOM) {
        vy = by + bh * (1.0f - ratio);
        vx = bx;
        vw = bw;
        vh = by + bh - vy;
    } else {
        vx = bx + bw * (1.0f - ratio);
        vy = by;
        vw = bx + bw - vx;
        vh = bh;
    }
    if (x < vx || x >= vx + vw || y < vy || y >= vy + vh) return false;

    const float ui_scale = sol_ui_system_scale(r->ui);
    const float header_h = SOL_UI_TERM_HEADER_PX * ui_scale;
    const float pad_v    = SOL_UI_TERM_PAD_V_PX  * ui_scale;
    const float pad_h    = SOL_UI_TERM_PAD_H_PX  * ui_scale;
    const float local_x  = (float)x - vx - pad_h;
    const float local_y  = (float)y - vy - header_h - pad_v;
    if (local_x < 0.0f || local_y < 0.0f) return false;

    const float cell_w = SOL_UI_TERM_CELL_W_PX * ui_scale;
    const float cell_h = SOL_UI_TERM_CELL_H_PX * ui_scale;
    if (cell_w <= 0.0f || cell_h <= 0.0f) return false;

    if (out_col) *out_col = (int)(local_x / cell_w);
    if (out_row) *out_row = (int)(local_y / cell_h);
    return true;
}

/* Printable keys arrive twice from GLFW — once as KEY and again decoded
   through CHAR. We only act on the CHAR copy so dead-keys / IME work. */
/* Return true when key falls in the printable ASCII range (32–126). */
static bool key_is_printable_alpha(SolKeyCode key)
{
    return (key >= 32 && key <= 126);
}

/* Settle the cursor into view + ask the UI to rebuild the buffer area. */
/*
 * After a buffer edit, scroll the cursor into the visible viewport and
 * request a buffer-area UI rebuild.  Uses the active leaf's geometry when
 * available, falling back to the full window dimensions.
 *
 * r   The input router providing geometry and UI references.
 * tb  The text buffer that was edited.
 */
static void post_edit_settle(SolInputRouter *r, SolTextBuffer *tb)
{
    if (!r || !tb) return;
    Ca_Window *win = sol_ui_system_primary_window(r->ui);
    const float ui_scale = sol_ui_system_scale(r->ui);
    SolBufferNodeId leaf_id = sol_buffer_active_leaf(r->buffers);
    SolBufferRect root_rect = {0};
    SolBufferRect leaf_rect = {0};
    if (leaf_id != 0u &&
        sol_ui_system_buffer_area_rect(r->ui, &root_rect.x, &root_rect.y, &root_rect.w, &root_rect.h) &&
        sol_buffer_leaf_geometry(r->buffers, leaf_id, &root_rect,
                                 SOL_UI_PANEL_GAP_PX * ui_scale, &leaf_rect)) {
        leaf_rect.h -= SOL_UI_BUFFER_TAB_STRIP_HEIGHT * ui_scale;
        if (leaf_rect.h < 0.0f) leaf_rect.h = 0.0f;
        const int viewport_full =
            sol_text_view_visible_lines_for_height(leaf_rect.h, ui_scale);
        int viewport = viewport_full - 2;
        if (viewport < 1) viewport = 1;
        const int viewport_cols = sol_text_view_visible_cols_for_width(
            leaf_rect.w, ui_scale, router_glyph_advance_px(win));
        int scroll_top  = sol_buffer_leaf_scroll_top(r->buffers, leaf_id);
        int scroll_left = sol_buffer_leaf_scroll_left(r->buffers, leaf_id);
        sol_text_buffer_ensure_cursor_visible_2d_ex(
            tb, viewport, viewport_cols, &scroll_top, &scroll_left);
        sol_buffer_set_leaf_scroll_top(r->buffers, leaf_id, scroll_top);
        sol_buffer_set_leaf_scroll_left(r->buffers, leaf_id, scroll_left);
        sol_ui_system_invalidate_buffer_area(r->ui);
        return;
    }

    /* Fallback: no leaf geometry — use window dimensions and active leaf. */
    int win_w = 0, win_h = 0;
    sol_ui_system_window_size(r->ui, &win_w, &win_h);
    if (win_w <= 0) win_w = 800;
    if (win_h <= 0) win_h = 600;
    const int viewport_full = sol_text_view_visible_lines(win_h, ui_scale);
    int viewport = viewport_full - 2;
    if (viewport < 1) viewport = 1;
    const int viewport_cols = sol_text_view_visible_cols_for_width(
        (float)win_w, ui_scale, router_glyph_advance_px(win));
    if (leaf_id != 0u) {
        int scroll_top  = sol_buffer_leaf_scroll_top(r->buffers, leaf_id);
        int scroll_left = sol_buffer_leaf_scroll_left(r->buffers, leaf_id);
        sol_text_buffer_ensure_cursor_visible_2d_ex(
            tb, viewport, viewport_cols, &scroll_top, &scroll_left);
        sol_buffer_set_leaf_scroll_top(r->buffers, leaf_id, scroll_top);
        sol_buffer_set_leaf_scroll_left(r->buffers, leaf_id, scroll_left);
    } else {
        sol_text_buffer_ensure_cursor_visible_2d(tb, viewport, viewport_cols);
    }
    sol_ui_system_invalidate_buffer_area(r->ui);
}

/* Handle non-printable / editing keys for the active text buffer. */
/*
 * Dispatch a non-printable editing key to the active text buffer.
 * Modifier chords other than Shift are ignored here; they belong to the
 * command-flow system.
 *
 * r     The input router providing the buffer system reference.
 * key   The key code to handle (arrow, Home, End, Backspace, Delete, Enter).
 * mods  Active modifier mask.
 * Returns true when the key was handled and the buffer was modified.
 */
static bool handle_text_buffer_key(SolInputRouter *r,
                                   SolKeyCode key, SolModifierMask mods)
{
    SolTextBuffer *tb = sol_text_buffer_active(r->buffers);
    if (!tb) return false;

    /* Modifier-bearing chords (Cmd-S etc.) belong to the flow system.
       Exception: Shift is allowed through for navigation keys so that
       Shift+Arrow/Home/End extend the selection. */
    const bool shift = (mods & SOL_MOD_SHIFT) != 0u;
    const bool has_non_shift_chord =
        (mods & (SOL_MOD_CTRL | SOL_MOD_SUPER | SOL_MOD_ALT)) != 0u;
    if (has_non_shift_chord) return false;

    bool handled = false;
    switch (key) {
    case SOL_KEY_LEFT:
        sol_text_buffer_move_cursor_sel(tb, -1,  0, false, shift);
        handled = true; break;
    case SOL_KEY_RIGHT:
        sol_text_buffer_move_cursor_sel(tb, +1,  0, false, shift);
        handled = true; break;
    case SOL_KEY_UP:
        sol_text_buffer_move_cursor_sel(tb,  0, -1, true,  shift);
        handled = true; break;
    case SOL_KEY_DOWN:
        sol_text_buffer_move_cursor_sel(tb,  0, +1, true,  shift);
        handled = true; break;
    case SOL_KEY_HOME:
        sol_text_buffer_move_line_start_sel(tb, shift);
        handled = true; break;
    case SOL_KEY_END:
        sol_text_buffer_move_line_end_sel(tb, shift);
        handled = true; break;
    case SOL_KEY_BACKSPACE: if (!shift) handled = sol_text_buffer_backspace(tb);      break;
    case SOL_KEY_DELETE:    if (!shift) handled = sol_text_buffer_delete_forward(tb); break;
    case SOL_KEY_ENTER:     if (!shift) handled = sol_text_buffer_insert_newline(tb); break;
    default: break;
    }

    if (handled) post_edit_settle(r, tb);
    return handled;
}

/* ------------------------------------------------------------------ */
/* Causality event handlers                                            */
/* ------------------------------------------------------------------ */

/*
 * Handle CA_EVENT_KEY events: translate to SolInputEvent, forward to the
 * UI system and input system, and dispatch non-printable editing keys to
 * the active text buffer.  Latches a one-shot text-input suppression when
 * the UI consumes a printable key chord.
 *
 * When the terminal panel has keyboard focus, ALL key events (including
 * ESC) are forwarded directly to the PTY so the running program can handle
 * them itself. The leader key is the only exception: it is intercepted to
 * arm/drive Sol's which-key popup instead of reaching the shell.
 */
static void on_key(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) {
        r->buffer_input_active = false;
        r->suppress_next_text_input = false;
        return;
    }

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

    SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
    const bool term_focused = tmgr && sol_terminal_manager_focused(tmgr);

    if (term_focused) {
        /* KEY_UP: only needed for leader tap detection (Ctrl released alone). */
        if (ie.type == SOL_INPUT_EVENT_KEY_UP) {
            sol_ui_system_handle_input_event(r->ui, &ie);
            sol_input_system_process_event(r->input, &ie);
            return;
        }

        if (ie.type == SOL_INPUT_EVENT_KEY_DOWN) {
            /* While a leader sequence is in progress route through the chord
               system — chord steps must not go to the PTY mid-sequence. */
            if (sol_ui_system_is_leader_active(r->ui)) {
                const bool ui_consumed = sol_ui_system_handle_input_event(r->ui, &ie);
                sol_input_system_process_event(r->input, &ie);
                if (ui_consumed && key_is_printable_alpha(ie.data.key.key)) {
                    r->suppress_next_text_input = true;
                }
                return;
            }

            /* Leader key down: arm tap detector, do not send to PTY. */
            if (sol_ui_is_leader_key(r->ui, ie.data.key.key)) {
                sol_ui_system_handle_input_event(r->ui, &ie);
                sol_input_system_process_event(r->input, &ie);
                return;
            }

            /* Paste intercept: Cmd+V (macOS Super+V) or Ctrl+Shift+V (Linux/Win).
               Both paste the system clipboard into the PTY with bracketed-paste
               framing when the application has enabled XTerm ?2004 mode.
               Must be checked before the generic has_ctrl_alt PTY-forward path
               so the paste chord is consumed here and never reaches send_key. */
            const SolModifierMask paste_mods = ie.data.key.modifiers;
            const SolKeyCode      paste_key  = ie.data.key.key;
            const bool is_super_v =
                (paste_mods == SOL_MOD_SUPER) && (paste_key == 'V' || paste_key == 'v');
            const bool is_ctrl_shift_v =
                ((paste_mods & (SOL_MOD_CTRL | SOL_MOD_SHIFT)) ==
                 (SOL_MOD_CTRL | SOL_MOD_SHIFT)) &&
                !(paste_mods & (SOL_MOD_ALT | SOL_MOD_SUPER)) &&
                (paste_key == 'V' || paste_key == 'v');
            if (is_super_v || is_ctrl_shift_v) {
                SolTerminal *term = sol_terminal_manager_active(tmgr);
                if (term) {
                    Ca_Window *win = sol_ui_system_primary_window(r->ui);
                    const char *text = win ? ca_clipboard_get_text(win) : NULL;
                    if (text && text[0] != '\0') {
                        sol_terminal_set_view_scroll(term, 0);
                        sol_terminal_paste(term, text, strlen(text));
                    }
                }
                r->suppress_next_text_input = true;
                return;
            }

            /* Everything else (Ctrl+C, Ctrl+Z, ESC, arrows, etc.) -> PTY.
               Unmodified printable chars are handled by on_char so Sol's
               app-level text path stays the single source of normal typing. */
            const bool has_ctrl_alt =
                (ie.data.key.modifiers &
                 (SOL_MOD_CTRL | SOL_MOD_ALT | SOL_MOD_SUPER)) != 0u;
            if (!key_is_printable_alpha(ie.data.key.key) || has_ctrl_alt) {
                SolTerminal *term = sol_terminal_manager_active(tmgr);
                if (term) {
                    sol_terminal_set_view_scroll(term, 0);
                    sol_terminal_send_key(term, ie.data.key.key,
                                         ie.data.key.modifiers);
                }
                if (has_ctrl_alt && key_is_printable_alpha(ie.data.key.key)) {
                    r->suppress_next_text_input = true;
                }
            }
            return;
        }
        return;
    }

    /* Buffer / non-terminal path: full UI event routing. */
    const bool ui_consumed = sol_ui_system_handle_input_event(r->ui, &ie);
    sol_input_system_process_event(r->input, &ie);

    if (ui_consumed && ie.type == SOL_INPUT_EVENT_KEY_DOWN &&
        key_is_printable_alpha(ie.data.key.key)) {
        r->suppress_next_text_input = true;
    }

    if (ie.type != SOL_INPUT_EVENT_KEY_DOWN) return;
    if (ui_consumed) return;
    if (sol_ui_system_is_leader_active(r->ui)) return;
    if (!r->buffer_input_active) return;
    if (key_is_printable_alpha(ie.data.key.key)) return;
    handle_text_buffer_key(r, ie.data.key.key, ie.data.key.modifiers);
}

/*
 * Handle CA_EVENT_CHAR (decoded text input) events: insert the codepoint
 * into the active text buffer unless suppressed (by a consumed command chord),
 * the leader popup is active, or the buffer input region is not focused.
 *
 * When the terminal has focus, printable codepoints are UTF-8 encoded and
 * written directly to the PTY.
 */
static void on_char(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) {
        r->buffer_input_active = false;
        r->suppress_next_text_input = false;
        return;
    }

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_TEXT_INPUT;
    ie.data.text.codepoint = ev->character.codepoint;
    sol_input_system_process_event(r->input, &ie);

    if (r->suppress_next_text_input) {
        r->suppress_next_text_input = false;
        return;
    }

    /* While a leader sequence is in progress the char belongs to the chord,
       not to the terminal or the buffer. */
    if (sol_ui_system_is_leader_active(r->ui)) return;

    /* Terminal-focused path: send codepoint to PTY as UTF-8. */
    SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
    if (tmgr && sol_terminal_manager_focused(tmgr)) {
        const uint32_t cp = ev->character.codepoint;
        if (cp != 0u && cp >= 0x20u && cp != 0x7Fu) {
            SolTerminal *term = sol_terminal_manager_active(tmgr);
            if (term) {
                sol_terminal_set_view_scroll(term, 0);
                char utf8[5] = {0};
                if (cp < 0x80u) {
                    utf8[0] = (char)cp;
                    sol_terminal_send_text(term, utf8, 1u);
                } else if (cp < 0x800u) {
                    utf8[0] = (char)(0xC0u | (cp >> 6));
                    utf8[1] = (char)(0x80u | (cp & 0x3Fu));
                    sol_terminal_send_text(term, utf8, 2u);
                } else if (cp < 0x10000u) {
                    utf8[0] = (char)(0xE0u | (cp >> 12));
                    utf8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    utf8[2] = (char)(0x80u | (cp & 0x3Fu));
                    sol_terminal_send_text(term, utf8, 3u);
                } else {
                    utf8[0] = (char)(0xF0u | (cp >> 18));
                    utf8[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                    utf8[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    utf8[3] = (char)(0x80u | (cp & 0x3Fu));
                    sol_terminal_send_text(term, utf8, 4u);
                }
            }
        }
        return;
    }

    if (!r->buffer_input_active) return;
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

/*
 * Handle CA_EVENT_MOUSE_BUTTON events: update buffer_input_active based on
 * whether the click landed inside a buffer pane, and forward the event to
 * the input system.
 */
static void on_mouse_button(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) {
        r->buffer_input_active = false;
        return;
    }

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

    /* Mouse-aware TUIs (e.g. an editor's mouse-driven selection, an Ink app
       with clickable rows) get first refusal on clicks inside the terminal
       grid. Falls through to normal Sol routing (scrollback, buffer focus)
       when the app hasn't enabled mouse tracking. */
    SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
    SolTerminal *term = tmgr ? sol_terminal_manager_active(tmgr) : NULL;
    int col = 0, row = 0;
    if (term && sol_terminal_wants_mouse(term) &&
        ev->mouse_button.button <= 2u &&
        terminal_cell_at_point(r, r->mouse_x, r->mouse_y, &col, &row)) {
        /* xterm mouse-report button order is left/middle/right; GLFW's is
           left/right/middle — remap index 1 and 2. */
        static const int k_btn_remap[3] = { 0, 2, 1 };
        const int report_btn = k_btn_remap[ev->mouse_button.button];
        const SolTermMouseAction action =
            (ie.type == SOL_INPUT_EVENT_MOUSE_UP) ? SOL_TERM_MOUSE_RELEASE
                                                   : SOL_TERM_MOUSE_PRESS;
        sol_terminal_send_mouse(term, col, row, report_btn, action,
                                (uint8_t)ie.data.mouse_button.modifiers);
        r->terminal_mouse_down = (action == SOL_TERM_MOUSE_PRESS);
        return;
    }
    r->terminal_mouse_down = false;

    sol_input_system_process_event(r->input, &ie);

    if (ie.type == SOL_INPUT_EVENT_MOUSE_DOWN) {
        r->buffer_input_active = point_in_active_buffer_leaf(
            r, r->mouse_x, r->mouse_y, NULL, NULL);
        if (!r->buffer_input_active) {
            r->horizontal_scroll_remainder = 0.0;
        }
    }
}

/*
 * Handle CA_EVENT_MOUSE_MOVE events: cache the pointer position and forward
 * the event to the input system.
 */
static void on_mouse_move(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) return;

    r->mouse_x = ev->mouse_pos.x;
    r->mouse_y = ev->mouse_pos.y;

    if (r->terminal_mouse_down) {
        SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
        SolTerminal *term = tmgr ? sol_terminal_manager_active(tmgr) : NULL;
        int col = 0, row = 0;
        if (term && sol_terminal_wants_mouse(term) &&
            terminal_cell_at_point(r, r->mouse_x, r->mouse_y, &col, &row)) {
            sol_terminal_send_mouse(term, col, row, 0,
                                    SOL_TERM_MOUSE_MOVE, SOL_MOD_NONE);
        }
        return;
    }

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_MOUSE_MOVE;
    ie.data.mouse_move.x = ev->mouse_pos.x;
    ie.data.mouse_move.y = ev->mouse_pos.y;
    sol_input_system_process_event(r->input, &ie);
}

/*
 * Handle CA_EVENT_MOUSE_SCROLL events: apply vertical and horizontal scroll
 * deltas to the buffer under the pointer using natural-scroll convention
 * (dy > 0 moves content up).  Forwards the event to the input system first.
 */
static void on_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) {
        r->buffer_input_active = false;
        r->horizontal_scroll_remainder = 0.0;
        return;
    }

    SolInputEvent ie = {0};
    ie.type = SOL_INPUT_EVENT_MOUSE_SCROLL;
    ie.data.mouse_scroll.x = (float)ev->mouse_scroll.dx;
    ie.data.mouse_scroll.y = (float)ev->mouse_scroll.dy;
    sol_input_system_process_event(r->input, &ie);

    if (!r->buffers || !r->ui) return;

    int win_w = 0, win_h = 0;
    sol_ui_system_window_size(r->ui, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    /* Route vertical scroll to the terminal when the mouse cursor is over the
       terminal panel, regardless of keyboard focus.  A mouse-aware app (one
       that enabled DECSET 1000/1002 — almost always full-screen/alt-screen,
       e.g. less, htop, an Ink list) gets wheel events reported as button
       64/65 clicks instead of Sol's own scrollback, since such apps own
       their own scroll position and Sol's scrollback is meaningless on the
       alt screen.  Otherwise: natural scroll, dy > 0 shows older content. */
    SolTerminalManager *tmgr = sol_ui_system_terminal_manager(r->ui);
    if (tmgr && sol_terminal_manager_visible(tmgr) && ev->mouse_scroll.dy != 0.0) {
        SolTerminal *term = sol_terminal_manager_active(tmgr);
        int col = 0, row = 0;
        const bool over_terminal =
            term && terminal_cell_at_point(r, r->mouse_x, r->mouse_y, &col, &row);
        if (over_terminal && sol_terminal_wants_mouse(term)) {
            const SolTermMouseAction action = ev->mouse_scroll.dy > 0.0
                ? SOL_TERM_MOUSE_WHEEL_UP : SOL_TERM_MOUSE_WHEEL_DOWN;
            sol_terminal_send_mouse(term, col, row, 0, action, SOL_MOD_NONE);
            return;
        }
        if (over_terminal) {
            int delta = (int)(ev->mouse_scroll.dy * 3.0);
            if (delta == 0) delta = ev->mouse_scroll.dy > 0.0 ? 1 : -1;
            sol_terminal_set_view_scroll(term,
                sol_terminal_view_scroll(term) + delta);
            sol_ui_system_terminal_notify(r->ui);
            return;
        }
    }

    SolBufferRect root_rect = {0};
    SolBufferNodeId target_leaf = 0u;
    const double mx = r->mouse_x, my = r->mouse_y;
    if (!point_in_active_buffer_leaf(r, mx, my, &root_rect, &target_leaf)) {
        r->horizontal_scroll_remainder = 0.0;
        return;
    }

    SolBufferId target = sol_buffer_leaf_buffer(r->buffers, target_leaf);
    if (target == 0u) return;

    SolBuffer *buf = sol_buffer_get(r->buffers, target);
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return;

    SolBufferRect leaf_rect = {0};
    Ca_Window *win = sol_ui_system_primary_window(r->ui);
    const float scale = sol_ui_system_scale(r->ui);
    if (target_leaf != 0u &&
        sol_buffer_leaf_geometry(r->buffers, target_leaf, &root_rect,
                                 SOL_UI_PANEL_GAP_PX * scale, &leaf_rect)) {
        /* The pane-local tab strip owns wheel gestures in its top band. */
        if ((float)r->mouse_y < leaf_rect.y + SOL_UI_BUFFER_TAB_STRIP_HEIGHT * scale)
            return;
        leaf_rect.h -= SOL_UI_BUFFER_TAB_STRIP_HEIGHT * scale;
        if (leaf_rect.h < 0.0f) leaf_rect.h = 0.0f;
        const int rendered =
            sol_text_view_visible_lines_for_height(leaf_rect.h, scale);
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

        int cur_top  = sol_buffer_leaf_scroll_top(r->buffers, target_leaf);
        int cur_left = sol_buffer_leaf_scroll_left(r->buffers, target_leaf);
        int new_top  = cur_top + delta;
        if (new_top < 0) new_top = 0;
        if (new_top > max_top) new_top = max_top;

        int new_left = cur_left;
        if (ev->mouse_scroll.dx != 0.0) {
            const int dx = horizontal_scroll_delta(r, ev->mouse_scroll.dx);
            new_left += dx;
            if (new_left < 0) {
                new_left = 0;
                r->horizontal_scroll_remainder = 0.0;
            }
        }

        if (new_top != cur_top || new_left != cur_left) {
            sol_buffer_set_leaf_scroll_top(r->buffers, target_leaf, new_top);
            sol_buffer_set_leaf_scroll_left(r->buffers, target_leaf, new_left);
            sol_ui_system_invalidate_buffer_area(r->ui);
        }
        return;
    }

    /* Fallback path: no leaf geometry available — scroll the active leaf. */
    const SolBufferNodeId active_leaf = sol_buffer_active_leaf(r->buffers);
    const int rendered = sol_text_view_visible_lines(win_h, scale);
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

    int cur_top  = active_leaf ? sol_buffer_leaf_scroll_top(r->buffers, active_leaf) : 0;
    int cur_left = active_leaf ? sol_buffer_leaf_scroll_left(r->buffers, active_leaf) : 0;
    int new_top  = cur_top + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;

    int new_left = cur_left;
    if (ev->mouse_scroll.dx != 0.0) {
        const int dx = horizontal_scroll_delta(r, ev->mouse_scroll.dx);
        new_left += dx;
        if (new_left < 0) {
            new_left = 0;
            r->horizontal_scroll_remainder = 0.0;
        }
    }

    if (new_top != cur_top || new_left != cur_left) {
        if (active_leaf) {
            sol_buffer_set_leaf_scroll_top(r->buffers, active_leaf, new_top);
            sol_buffer_set_leaf_scroll_left(r->buffers, active_leaf, new_left);
        }
        sol_ui_system_invalidate_buffer_area(r->ui);
    }
}

/* Forward a window-close event to the UI system. */
static void on_window_close(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    sol_ui_system_on_window_close(r->ui, ev->window);
}

/* Forward a primary-window resize event to the UI system. */
static void on_window_resize(const Ca_Event *ev, void *user_data)
{
    SolInputRouter *r = (SolInputRouter *)user_data;
    if (!ev || !r) return;
    if (!event_is_from_primary_window(r, ev)) return;
    sol_ui_system_on_window_resize(r->ui, ev->resize.width, ev->resize.height);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Create a SolInputRouter and register all seven Causality event handlers
 * (key, char, mouse button, mouse move, mouse scroll, window close, window
 * resize) with the given instance.
 *
 * instance  Causality instance to hook events on.
 * ui        UI system that receives forwarded events.
 * input     Low-level input system for event processing.
 * buffers   Buffer system used for text editing and scroll.
 * Returns   The new router, or NULL on allocation failure or invalid args.
 */
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
    r->buffer_input_active = sol_buffer_active_buffer(buffers) != 0u;

    ca_event_set_handler(instance, CA_EVENT_KEY,           on_key,           r);
    ca_event_set_handler(instance, CA_EVENT_CHAR,          on_char,          r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON,  on_mouse_button,  r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE,    on_mouse_move,    r);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL,  on_mouse_scroll,  r);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE,  on_window_close,  r);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_window_resize, r);
    return r;
}

/*
 * Deregister all Causality event handlers and free the router.
 *
 * router  The router to destroy (safe to call with NULL).
 */
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
