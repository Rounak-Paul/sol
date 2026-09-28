// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* buffer_find.c — incremental find inside the focused text buffer.
 *
 * The query is typed into the status bar. Every change re-scans the buffer,
 * previews the nearest match as a selection and highlights the others;
 * Up/Down cycle, Enter lands the caret on the match, Esc restores the
 * original caret and scroll. The session observes the buffer event bus so
 * edits, focus changes and closes of the target buffer keep it consistent.
 */

#include "sol_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_event.h"
#include "sol_text_buffer.h"

#define SOL_UI_BUFFER_FIND_INITIAL_MATCHES 1024u

/*
 * Resolve the text buffer the find session targets.
 *
 * ui  The UI system owning the session.
 * Returns the target text buffer, or NULL when it no longer exists.
 */
static SolTextBuffer *buffer_find_target(SolUISystem *ui)
{
    SolBuffer *buffer = sol_buffer_get(ui->buffers, ui->buffer_find.buffer_id);
    return buffer ? sol_text_buffer_state(buffer) : NULL;
}

/*
 * Repaint the surfaces the find session draws on: the status-bar prompt
 * and the match highlights in the buffer area.
 *
 * ui  The UI system owning the session.
 */
static void buffer_find_invalidate(SolUISystem *ui)
{
    if (ui->primary_window) ca_window_invalidate_status_bar(ui->primary_window);
    sol_ui_system_invalidate_buffer_area(ui);
}

/*
 * Re-scan the target buffer for the current query, growing the match
 * array (up to SOL_UI_BUFFER_FIND_MAX_MATCHES) when it is too small.
 *
 * find  The find session.
 * tb    The target text buffer.
 */
static void buffer_find_scan(SolUIBufferFind *find, const SolTextBuffer *tb)
{
    find->match_count = 0u;
    find->match_total = 0u;
    if (find->query_len == 0u) return;

    size_t total = sol_text_buffer_find_all(
        tb, (const uint8_t *)find->query, find->query_len,
        find->matches, find->match_capacity);
    if (total > find->match_capacity &&
        find->match_capacity < SOL_UI_BUFFER_FIND_MAX_MATCHES) {
        const size_t wanted = total < SOL_UI_BUFFER_FIND_MAX_MATCHES
            ? total : SOL_UI_BUFFER_FIND_MAX_MATCHES;
        size_t *grown = (size_t *)realloc(find->matches, wanted * sizeof(size_t));
        if (grown) {
            find->matches = grown;
            find->match_capacity = wanted;
            total = sol_text_buffer_find_all(
                tb, (const uint8_t *)find->query, find->query_len,
                find->matches, find->match_capacity);
        }
    }
    find->match_total = total;
    find->match_count = total < find->match_capacity ? total : find->match_capacity;
}

/*
 * Return the index of the first stored match starting at or after byte,
 * wrapping to the first match when none follows.
 *
 * find  The find session (match_count must be > 0).
 * byte  Byte offset to search from.
 */
static size_t buffer_find_index_from(const SolUIBufferFind *find, size_t byte)
{
    size_t lo = 0u;
    size_t hi = find->match_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        if (find->matches[mid] < byte) lo = mid + 1u;
        else hi = mid;
    }
    return lo < find->match_count ? lo : 0u;
}

/*
 * Restore the caret and selection captured when the session opened.
 *
 * find  The find session.
 * tb    The target text buffer.
 */
static void buffer_find_restore_caret(const SolUIBufferFind *find, SolTextBuffer *tb)
{
    if (find->origin_has_selection) {
        sol_text_buffer_select_range(tb, find->origin_anchor, find->origin_cursor);
    } else {
        sol_text_buffer_set_cursor_byte(tb, find->origin_cursor);
    }
}

/*
 * Preview the current match as a selection with the caret at its end.
 *
 * find  The find session (match_count must be > 0).
 * tb    The target text buffer.
 */
static void buffer_find_select_current(const SolUIBufferFind *find, SolTextBuffer *tb)
{
    const size_t start = find->matches[find->current];
    sol_text_buffer_select_range(tb, start, start + find->query_len);
}

/*
 * Re-run the query after it changed and preview the match nearest the
 * position the session started from; with no match the original caret
 * comes back.
 *
 * ui  The UI system owning the session.
 * Returns the input outcome for the router.
 */
static SolUIFindInput buffer_find_requery(SolUISystem *ui)
{
    SolUIBufferFind *find = &ui->buffer_find;
    SolTextBuffer *tb = buffer_find_target(ui);
    if (!tb) {
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
        return SOL_UI_FIND_INPUT_CONSUMED;
    }
    buffer_find_scan(find, tb);
    if (find->match_count > 0u) {
        find->current = buffer_find_index_from(find, find->origin_start);
        buffer_find_select_current(find, tb);
    } else {
        find->current = 0u;
        buffer_find_restore_caret(find, tb);
    }
    buffer_find_invalidate(ui);
    return SOL_UI_FIND_INPUT_MOVED;
}

/*
 * Step the current match forward or backward, wrapping at either end.
 *
 * ui         The UI system owning the session.
 * direction  +1 for the next match, -1 for the previous one.
 * Returns the input outcome for the router.
 */
static SolUIFindInput buffer_find_step(SolUISystem *ui, int direction)
{
    SolUIBufferFind *find = &ui->buffer_find;
    if (find->match_count == 0u) return SOL_UI_FIND_INPUT_CONSUMED;
    SolTextBuffer *tb = buffer_find_target(ui);
    if (!tb) {
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
        return SOL_UI_FIND_INPUT_CONSUMED;
    }
    if (direction > 0) {
        find->current = find->current + 1u < find->match_count ? find->current + 1u : 0u;
    } else {
        find->current = find->current > 0u ? find->current - 1u : find->match_count - 1u;
    }
    buffer_find_select_current(find, tb);
    buffer_find_invalidate(ui);
    return SOL_UI_FIND_INPUT_MOVED;
}

/*
 * Drop the last UTF-8 codepoint of the query.
 *
 * find  The find session.
 * Returns true when a codepoint was removed.
 */
static bool buffer_find_pop_codepoint(SolUIBufferFind *find)
{
    if (find->query_len == 0u) return false;
    size_t len = find->query_len - 1u;
    while (len > 0u && ((uint8_t)find->query[len] & 0xC0u) == 0x80u) --len;
    find->query_len = len;
    find->query[len] = '\0';
    return true;
}

/*
 * Event-bus observer: keep matches in sync with edits to the target buffer.
 * The caret is left where the edit put it; the current match becomes the
 * first one at or after it.
 *
 * event      SOL_EVENT_TEXT_EDITED carrying a SolTextEditedPayload.
 * user_data  The SolUISystem owning the session.
 * Returns false so other observers still receive the event.
 */
static bool buffer_find_on_text_edited(const SolEvent *event, void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffer_find.active || !event ||
        event->payload_size < sizeof(SolTextEditedPayload)) {
        return false;
    }
    const SolTextEditedPayload *edit = (const SolTextEditedPayload *)event->payload;
    if (edit->buffer_id != ui->buffer_find.buffer_id) return false;

    SolUIBufferFind *find = &ui->buffer_find;
    SolTextBuffer *tb = buffer_find_target(ui);
    if (!tb) {
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
        return false;
    }
    buffer_find_scan(find, tb);
    find->current = find->match_count > 0u
        ? buffer_find_index_from(find, sol_text_buffer_cursor_byte(tb))
        : 0u;
    buffer_find_invalidate(ui);
    return false;
}

/*
 * Event-bus observer: end the session when another buffer takes focus or
 * the target buffer closes. The target is never touched, since a closing
 * buffer may already be torn down.
 *
 * event      SOL_EVENT_BUFFER_FOCUSED or SOL_EVENT_BUFFER_CLOSED carrying a
 *            SolBufferEventPayload.
 * user_data  The SolUISystem owning the session.
 * Returns false so other observers still receive the event.
 */
static bool buffer_find_on_buffer_changed(const SolEvent *event, void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffer_find.active || !event ||
        event->payload_size < sizeof(SolBufferEventPayload)) {
        return false;
    }
    const SolBufferEventPayload *payload = (const SolBufferEventPayload *)event->payload;
    const bool closed = event->name && strcmp(event->name, SOL_EVENT_BUFFER_CLOSED) == 0;
    const bool is_target = payload->buffer_id == ui->buffer_find.buffer_id;
    if (closed ? is_target : !is_target) {
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
    }
    return false;
}

/*
 * Subscribe the session's observers on the buffer system's event bus.
 *
 * ui  The UI system owning the session.
 */
static void buffer_find_subscribe(SolUISystem *ui)
{
    SolUIBufferFind *find = &ui->buffer_find;
    find->bus = sol_buffer_event_bus(ui->buffers);
    if (!find->bus) return;
    find->edit_token = sol_event_bus_subscribe(find->bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
        .handler    = buffer_find_on_text_edited,
        .user_data  = ui,
    });
    find->focus_token = sol_event_bus_subscribe(find->bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_FOCUSED,
        .handler    = buffer_find_on_buffer_changed,
        .user_data  = ui,
    });
    find->close_token = sol_event_bus_subscribe(find->bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_CLOSED,
        .handler    = buffer_find_on_buffer_changed,
        .user_data  = ui,
    });
}

/*
 * Remove the session's observers from the event bus.
 *
 * find  The find session.
 */
static void buffer_find_unsubscribe(SolUIBufferFind *find)
{
    if (find->bus) {
        sol_event_bus_unsubscribe(find->bus, find->edit_token);
        sol_event_bus_unsubscribe(find->bus, find->focus_token);
        sol_event_bus_unsubscribe(find->bus, find->close_token);
    }
    find->bus = NULL;
    find->edit_token = find->focus_token = find->close_token = 0u;
}

/*
 * Start a find session on the active text buffer. Re-opening while a
 * session is active keeps the current query.
 *
 * ui  The UI system (must be the mounted project).
 * Returns true when a session is active afterwards.
 */
bool sol_ui_system_buffer_find_open(SolUISystem *ui)
{
    if (!ui || !ui->active) return false;
    SolUIBufferFind *find = &ui->buffer_find;
    const SolBufferId buffer_id = sol_buffer_active_buffer(ui->buffers);
    if (find->active) {
        if (find->buffer_id == buffer_id) return true;
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
    }

    SolTextBuffer *tb = sol_text_buffer_active(ui->buffers);
    if (!tb || buffer_id == 0u) return false;
    if (!find->matches) {
        find->matches = (size_t *)malloc(SOL_UI_BUFFER_FIND_INITIAL_MATCHES * sizeof(size_t));
        if (!find->matches) return false;
        find->match_capacity = SOL_UI_BUFFER_FIND_INITIAL_MATCHES;
    }

    const SolBufferNodeId leaf = sol_buffer_active_leaf(ui->buffers);
    size_t sel_start = 0u, sel_end = 0u;
    sol_text_buffer_selection_range(tb, &sel_start, &sel_end);
    find->origin_cursor = sol_text_buffer_cursor_byte(tb);
    find->origin_has_selection = sol_text_buffer_has_selection(tb);
    find->origin_anchor = find->origin_cursor == sel_start ? sel_end : sel_start;
    find->origin_start = sel_start;
    find->origin_leaf = leaf;
    find->origin_scroll_top = leaf ? sol_buffer_leaf_scroll_top(ui->buffers, leaf)
                                   : sol_text_buffer_scroll_top(tb);
    find->origin_scroll_left = leaf ? sol_buffer_leaf_scroll_left(ui->buffers, leaf)
                                    : sol_text_buffer_scroll_left(tb);
    find->buffer_id = buffer_id;
    find->query[0] = '\0';
    find->query_len = 0u;
    find->match_count = find->match_total = find->current = 0u;
    find->active = true;
    buffer_find_subscribe(ui);
    buffer_find_invalidate(ui);
    return true;
}

/*
 * End the find session.
 *
 * ui    The UI system owning the session.
 * mode  LAND puts the caret on the current match, CANCEL restores the
 *       original caret, selection and scroll, RELEASE leaves the buffer
 *       untouched.
 */
void sol_ui_system_buffer_find_close(SolUISystem *ui, SolUIFindClose mode)
{
    if (!ui || !ui->buffer_find.active) return;
    SolUIBufferFind *find = &ui->buffer_find;
    find->active = false;
    buffer_find_unsubscribe(find);

    SolTextBuffer *tb = mode == SOL_UI_FIND_CLOSE_RELEASE ? NULL : buffer_find_target(ui);
    if (tb && mode == SOL_UI_FIND_CLOSE_LAND && find->match_count > 0u) {
        sol_text_buffer_set_cursor_byte(tb, find->matches[find->current]);
    } else if (tb && mode == SOL_UI_FIND_CLOSE_CANCEL) {
        buffer_find_restore_caret(find, tb);
        if (find->origin_leaf != 0u) {
            sol_buffer_set_leaf_scroll_top(ui->buffers, find->origin_leaf,
                                           find->origin_scroll_top);
            sol_buffer_set_leaf_scroll_left(ui->buffers, find->origin_leaf,
                                            find->origin_scroll_left);
        } else {
            sol_text_buffer_set_scroll_top(tb, find->origin_scroll_top);
            sol_text_buffer_set_scroll_left(tb, find->origin_scroll_left);
        }
    }
    find->match_count = find->match_total = find->current = 0u;
    buffer_find_invalidate(ui);
}

/*
 * Release the session and its match storage. Safe to call repeatedly.
 *
 * ui  The UI system owning the session.
 */
void sol_ui_buffer_find_shutdown(SolUISystem *ui)
{
    if (!ui) return;
    sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_RELEASE);
    free(ui->buffer_find.matches);
    ui->buffer_find.matches = NULL;
    ui->buffer_find.match_capacity = 0u;
}

/*
 * Report whether a find session currently owns buffer keyboard input.
 *
 * ui  The UI system to query.
 */
bool sol_ui_system_buffer_find_active(const SolUISystem *ui)
{
    return ui && ui->buffer_find.active;
}

/*
 * Route a key press to the find session. Chords carrying Ctrl/Alt/Super
 * are left for the command system; every other key is owned by the
 * prompt so it never edits the buffer underneath.
 *
 * ui    The UI system owning the session.
 * key   Pressed key code.
 * mods  Active modifier mask.
 * Returns IGNORED when the session is inactive or the key is a chord,
 *         MOVED when the caret or selection changed, CONSUMED otherwise.
 */
SolUIFindInput sol_ui_system_buffer_find_key(SolUISystem *ui, SolKeyCode key,
                                             SolModifierMask mods)
{
    if (!ui || !ui->buffer_find.active) return SOL_UI_FIND_INPUT_IGNORED;
    if ((mods & (SOL_MOD_CTRL | SOL_MOD_ALT | SOL_MOD_SUPER)) != 0u) {
        return SOL_UI_FIND_INPUT_IGNORED;
    }

    SolUIBufferFind *find = &ui->buffer_find;
    switch (key) {
    case SOL_KEY_ESCAPE:
        sol_ui_system_buffer_find_close(ui, SOL_UI_FIND_CLOSE_CANCEL);
        return SOL_UI_FIND_INPUT_CONSUMED;
    case SOL_KEY_ENTER: {
        const bool landed = find->match_count > 0u;
        sol_ui_system_buffer_find_close(
            ui, landed ? SOL_UI_FIND_CLOSE_LAND : SOL_UI_FIND_CLOSE_CANCEL);
        return landed ? SOL_UI_FIND_INPUT_MOVED : SOL_UI_FIND_INPUT_CONSUMED;
    }
    case SOL_KEY_DOWN:
        return buffer_find_step(ui, +1);
    case SOL_KEY_UP:
        return buffer_find_step(ui, -1);
    case SOL_KEY_BACKSPACE:
        return buffer_find_pop_codepoint(find) ? buffer_find_requery(ui)
                                               : SOL_UI_FIND_INPUT_CONSUMED;
    default:
        return SOL_UI_FIND_INPUT_CONSUMED;
    }
}

/*
 * Append a typed codepoint to the query and re-run it. Control codes
 * other than TAB, DEL, surrogates and out-of-range values are ignored, as
 * is input that would overflow the query.
 *
 * ui  The UI system owning the session.
 * cp  Unicode codepoint from the text-input event.
 * Returns IGNORED when the session is inactive, MOVED after a re-query,
 *         CONSUMED when the codepoint was dropped.
 */
SolUIFindInput sol_ui_system_buffer_find_char(SolUISystem *ui, uint32_t cp)
{
    if (!ui || !ui->buffer_find.active) return SOL_UI_FIND_INPUT_IGNORED;
    if ((cp < 0x20u && cp != 0x09u) || cp == 0x7Fu || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return SOL_UI_FIND_INPUT_CONSUMED;
    }

    uint8_t utf8[4];
    size_t n;
    if (cp < 0x80u) {
        utf8[0] = (uint8_t)cp;
        n = 1u;
    } else if (cp < 0x800u) {
        utf8[0] = (uint8_t)(0xC0u | (cp >> 6));
        utf8[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 2u;
    } else if (cp < 0x10000u) {
        utf8[0] = (uint8_t)(0xE0u | (cp >> 12));
        utf8[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        utf8[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 3u;
    } else {
        utf8[0] = (uint8_t)(0xF0u | (cp >> 18));
        utf8[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        utf8[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        utf8[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 4u;
    }

    SolUIBufferFind *find = &ui->buffer_find;
    if (find->query_len + n > SOL_UI_BUFFER_FIND_QUERY_MAX) return SOL_UI_FIND_INPUT_CONSUMED;
    memcpy(find->query + find->query_len, utf8, n);
    find->query_len += n;
    find->query[find->query_len] = '\0';
    return buffer_find_requery(ui);
}

/*
 * Expose the match set for rendering highlights in one pane.
 *
 * ui           The UI system owning the session.
 * tb           Text buffer being rendered; only the target reports matches.
 * out_offsets  Receives the ascending match start offsets.
 * out_count    Receives the number of stored matches.
 * out_len      Receives the byte length shared by every match.
 * out_current  Receives the index of the current match.
 * Returns true when tb is the target and at least one match exists.
 */
bool sol_ui_system_buffer_find_matches(const SolUISystem *ui, const SolTextBuffer *tb,
                                       const size_t **out_offsets, size_t *out_count,
                                       size_t *out_len, size_t *out_current)
{
    if (!ui || !tb || !out_offsets || !out_count || !out_len || !out_current) return false;
    const SolUIBufferFind *find = &ui->buffer_find;
    if (!find->active || find->match_count == 0u) return false;
    const SolBuffer *buffer = sol_buffer_get_const(ui->buffers, find->buffer_id);
    if (!buffer || sol_text_buffer_state((SolBuffer *)buffer) != tb) return false;
    *out_offsets = find->matches;
    *out_count = find->match_count;
    *out_len = find->query_len;
    *out_current = find->current;
    return true;
}

/*
 * Emit the find prompt into the status bar's left section: a badge, the
 * typed query and the match position ("3/17", "3/262144+", "no matches").
 * Text is formatted into session-owned storage because Causality keeps
 * the pointers until the next status-bar build.
 *
 * ui  The UI system owning the session (must be active).
 */
void sol_ui_buffer_find_render_status(SolUISystem *ui)
{
    SolUIBufferFind *find = &ui->buffer_find;
    static const char badge_text[2] = { SOL_UI_STATUS_KIND_FIND, '\0' };

    snprintf(find->prompt_text, sizeof(find->prompt_text), "find: %s", find->query);
    if (find->query_len == 0u) {
        snprintf(find->position_text, sizeof(find->position_text), "type to search");
    } else if (find->match_count == 0u) {
        snprintf(find->position_text, sizeof(find->position_text), "no matches");
    } else {
        snprintf(find->position_text, sizeof(find->position_text), "%zu/%zu%s",
                 find->current + 1u, find->match_count,
                 find->match_total > find->match_count ? "+" : "");
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar-badge status-bar-badge-command",
    });
    ca_text(&(Ca_TextDesc){ .text = badge_text, .style = "status-bar-badge-text" });
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar-value",
    });
    ca_text(&(Ca_TextDesc){ .text = find->prompt_text, .style = "status-bar-text" });
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar-value",
    });
    ca_text(&(Ca_TextDesc){ .text = find->position_text, .style = "status-bar-text" });
    ca_div_end();
}
