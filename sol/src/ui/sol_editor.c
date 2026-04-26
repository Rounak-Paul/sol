// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_editor.c — editor implementation. Always-edit, no vim modes.
// Commands run via a leader-key node-flow tree (which-key style).

#include "sol_editor.h"
#include "sol_text_buffer.h"
#include "sol_file.h"

#include <causality.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   Forward decls of action handlers (defined below the table)
   ============================================================ */

static void act_save        (SolEditor *ed);
static void act_close       (SolEditor *ed);
static void act_new_scratch (SolEditor *ed);
static void act_buffer_next (SolEditor *ed);
static void act_buffer_prev (SolEditor *ed);
static void act_quit        (SolEditor *ed);

/* ============================================================
   Command flow table
   ============================================================
   Convention: paths are uppercase ASCII. Interior nodes (those with
   children) declare their own row with NULL action and a label.
   The popup auto-discovers children by prefix match.
   ============================================================ */

static const SolFlowEntry g_flow[] = {
    /* Top-level fast keys (single-letter, fire immediately) */
    { "S",  "Save",          act_save        },
    { "W",  "Close buffer",  act_close       },
    { "N",  "New scratch",   act_new_scratch },
    { "Q",  "Quit",          act_quit        },

    /* Buffer submenu */
    { "B",  "Buffer",        NULL            },
    { "BN", "Next",          act_buffer_next },
    { "BP", "Previous",      act_buffer_prev },
    { "BD", "Delete",        act_close       },

    /* File submenu (mirrors top-level for muscle-memory) */
    { "F",  "File",          NULL            },
    { "FS", "Save",          act_save        },
    { "FN", "New scratch",   act_new_scratch },
    { "FW", "Close",         act_close       },
    { "FQ", "Quit",          act_quit        },
};
static const size_t g_flow_count = sizeof(g_flow) / sizeof(g_flow[0]);

/* ============================================================
   State
   ============================================================ */

struct SolEditor {
    SolDoc docs[SOL_EDITOR_MAX_DOCS];
    int    active;

    /* Command-flow chord in progress (uppercase letters, NUL-terminated). */
    char   flow_prefix[SOL_EDITOR_FLOW_MAX];
    size_t flow_len;
};

/* ============================================================
   Lifecycle
   ============================================================ */

SolEditor *sol_editor_create(void)
{
    SolEditor *ed = (SolEditor *)calloc(1, sizeof(*ed));
    if (!ed) return NULL;
    ed->active = -1;
    return ed;
}

void sol_editor_destroy(SolEditor *ed)
{
    if (!ed) return;
    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
        if (ed->docs[i].in_use && ed->docs[i].text)
            sol_text_buffer_destroy(ed->docs[i].text);
    free(ed);
}

/* ============================================================
   Documents
   ============================================================ */

static int alloc_doc_slot(SolEditor *ed)
{
    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
        if (!ed->docs[i].in_use) return i;
    return -1;
}

static const char *path_basename(const char *path)
{
    if (!path || !*path) return "[scratch]";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int sol_editor_open_path(SolEditor *ed, const char *path)
{
    if (!ed || !path) return -1;

    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
        if (ed->docs[i].in_use && strcmp(ed->docs[i].path, path) == 0) {
            ed->active = i;
            return i;
        }

    int slot = alloc_doc_slot(ed);
    if (slot < 0) return -1;

    char  *bytes  = NULL;
    size_t length = 0;
    SolFileResult r = sol_file_read_all(path, 64u * 1024u * 1024u, &bytes, &length);
    SolTextBuffer *tb =
        (r == SOL_FILE_OK)
        ? sol_text_buffer_create_from_owned(bytes, length)
        : sol_text_buffer_create();
    if (!tb) return -1;

    SolDoc *d = &ed->docs[slot];
    memset(d, 0, sizeof(*d));
    snprintf(d->path,         sizeof(d->path),         "%s", path);
    snprintf(d->display_name, sizeof(d->display_name), "%s", path_basename(path));
    d->text   = tb;
    d->in_use = true;
    sol_text_buffer_mark_clean(tb);
    ed->active = slot;
    return slot;
}

int sol_editor_open_scratch(SolEditor *ed, const char *display_name)
{
    if (!ed) return -1;
    int slot = alloc_doc_slot(ed);
    if (slot < 0) return -1;
    SolTextBuffer *tb = sol_text_buffer_create();
    if (!tb) return -1;
    SolDoc *d = &ed->docs[slot];
    memset(d, 0, sizeof(*d));
    snprintf(d->display_name, sizeof(d->display_name), "%s",
             display_name ? display_name : "[scratch]");
    d->text   = tb;
    d->in_use = true;
    ed->active = slot;
    return slot;
}

bool sol_editor_close_doc(SolEditor *ed, int index)
{
    if (!ed || index < 0 || index >= SOL_EDITOR_MAX_DOCS) return false;
    SolDoc *d = &ed->docs[index];
    if (!d->in_use) return false;
    sol_text_buffer_destroy(d->text);
    memset(d, 0, sizeof(*d));
    if (ed->active == index) {
        ed->active = -1;
        for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
            if (ed->docs[i].in_use) { ed->active = i; break; }
    }
    return true;
}

bool sol_editor_save_doc(SolEditor *ed, int index)
{
    if (!ed || index < 0 || index >= SOL_EDITOR_MAX_DOCS) return false;
    SolDoc *d = &ed->docs[index];
    if (!d->in_use || d->path[0] == '\0') return false;

    char *cstr = sol_text_buffer_to_cstring(d->text);
    if (!cstr) return false;
    size_t len = sol_text_buffer_length(d->text);
    SolFileResult r = sol_file_write_all_atomic(d->path, cstr, len);
    free(cstr);
    if (r != SOL_FILE_OK) {
        fprintf(stderr, "[sol] save failed: %s (%s)\n",
                d->path, sol_file_result_str(r));
        return false;
    }
    sol_text_buffer_mark_clean(d->text);
    return true;
}

void sol_editor_set_active(SolEditor *ed, int index)
{
    if (!ed || index < 0 || index >= SOL_EDITOR_MAX_DOCS) return;
    if (ed->docs[index].in_use) ed->active = index;
}

int    sol_editor_active_index(const SolEditor *ed) { return ed ? ed->active : -1; }
size_t sol_editor_doc_count   (const SolEditor *ed)
{
    if (!ed) return 0;
    size_t n = 0;
    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
        if (ed->docs[i].in_use) n++;
    return n;
}
const SolDoc *sol_editor_doc(const SolEditor *ed, int i)
{
    if (!ed || i < 0 || i >= SOL_EDITOR_MAX_DOCS) return NULL;
    return ed->docs[i].in_use ? &ed->docs[i] : NULL;
}
SolDoc *sol_editor_doc_mut(SolEditor *ed, int i)
{
    if (!ed || i < 0 || i >= SOL_EDITOR_MAX_DOCS) return NULL;
    return ed->docs[i].in_use ? &ed->docs[i] : NULL;
}

const char *sol_editor_flow_prefix(const SolEditor *ed)
{
    return ed ? ed->flow_prefix : "";
}

/* ============================================================
   Action implementations
   ============================================================ */

static void act_save        (SolEditor *ed) { sol_editor_save_doc(ed, ed->active); }
static void act_close       (SolEditor *ed) { sol_editor_close_doc(ed, ed->active); }
static void act_new_scratch (SolEditor *ed) { sol_editor_open_scratch(ed, "[scratch]"); }

static void act_buffer_next(SolEditor *ed)
{
    if (!ed) return;
    int n = SOL_EDITOR_MAX_DOCS;
    for (int step = 1; step <= n; ++step) {
        int i = (ed->active + step) % n;
        if (i < 0) i += n;
        if (ed->docs[i].in_use) { ed->active = i; return; }
    }
}

static void act_buffer_prev(SolEditor *ed)
{
    if (!ed) return;
    int n = SOL_EDITOR_MAX_DOCS;
    for (int step = 1; step <= n; ++step) {
        int i = (ed->active - step) % n;
        if (i < 0) i += n;
        if (ed->docs[i].in_use) { ed->active = i; return; }
    }
}

static void act_quit(SolEditor *ed)
{
    /* Drop all docs; window stays open until user closes it. */
    if (!ed) return;
    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i)
        if (ed->docs[i].in_use) sol_editor_close_doc(ed, i);
}

/* ============================================================
   Flow lookup
   ============================================================ */

static const SolFlowEntry *flow_lookup_exact(const char *path)
{
    for (size_t i = 0; i < g_flow_count; ++i)
        if (strcmp(g_flow[i].path, path) == 0) return &g_flow[i];
    return NULL;
}

/* True if any entry has `path` as a strict prefix (i.e. it's an interior
   node with children). */
static bool flow_has_children(const char *path)
{
    size_t plen = strlen(path);
    for (size_t i = 0; i < g_flow_count; ++i) {
        const char *p = g_flow[i].path;
        if (strlen(p) > plen && strncmp(p, path, plen) == 0)
            return true;
    }
    return false;
}

static void flow_reset(SolEditor *ed)
{
    ed->flow_prefix[0] = '\0';
    ed->flow_len       = 0;
}

/* Try to advance the chord by one letter. Returns true if the event was
   consumed by the flow system (regardless of whether it matched). */
static bool flow_step(SolEditor *ed, char letter)
{
    if (ed->flow_len + 1 >= sizeof(ed->flow_prefix)) {
        flow_reset(ed);
        return true;
    }
    ed->flow_prefix[ed->flow_len++] = letter;
    ed->flow_prefix[ed->flow_len]   = '\0';

    const SolFlowEntry *e = flow_lookup_exact(ed->flow_prefix);
    if (e && e->action) { e->action(ed); flow_reset(ed); return true; }
    if (e || flow_has_children(ed->flow_prefix)) return true; /* keep popup */
    flow_reset(ed); /* dead-end */
    return true;
}

/* ============================================================
   UTF-8 + cursor helpers
   ============================================================ */

static size_t prev_char(const SolDoc *d, size_t c)
{
    if (!d || c == 0) return 0;
    char buf[1];
    do {
        c--;
        if (sol_text_buffer_read(d->text, c, buf, 1) == 0) break;
        if ((buf[0] & 0xC0) != 0x80) break;
    } while (c > 0);
    return c;
}

static size_t next_char(const SolDoc *d, size_t c)
{
    if (!d) return 0;
    size_t len = sol_text_buffer_length(d->text);
    if (c >= len) return len;
    char buf[1];
    sol_text_buffer_read(d->text, c, buf, 1);
    unsigned char b = (unsigned char)buf[0];
    size_t adv = (b < 0x80) ? 1
               : ((b & 0xE0) == 0xC0) ? 2
               : ((b & 0xF0) == 0xE0) ? 3
               : ((b & 0xF8) == 0xF0) ? 4 : 1;
    if (c + adv > len) adv = len - c;
    return c + adv;
}

#define MAX_VISIBLE_LINES 200

static size_t collect_line_starts(const SolDoc *d, size_t *out, size_t max)
{
    if (!d || !d->text || max == 0) return 0;
    size_t len = sol_text_buffer_length(d->text);
    out[0] = 0;
    size_t n = 1;
    char buf[4096];
    size_t pos = 0;
    while (pos < len && n < max) {
        size_t got = sol_text_buffer_read(d->text, pos, buf, sizeof(buf));
        if (got == 0) break;
        for (size_t i = 0; i < got && n < max; ++i)
            if (buf[i] == '\n') out[n++] = pos + i + 1;
        pos += got;
    }
    return n;
}

static void cursor_to_lc(const SolDoc *d, size_t cursor,
                         size_t *line_out, size_t *col_out)
{
    *line_out = 0; *col_out = 0;
    if (!d || !d->text) return;
    size_t len = sol_text_buffer_length(d->text);
    if (cursor > len) cursor = len;
    char buf[4096];
    size_t pos = 0, line = 0, last_nl = 0;
    while (pos < cursor) {
        size_t want = cursor - pos;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t got = sol_text_buffer_read(d->text, pos, buf, want);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i)
            if (buf[i] == '\n') { line++; last_nl = pos + i + 1; }
        pos += got;
    }
    *line_out = line;
    *col_out  = cursor - last_nl;
}

static size_t lc_to_cursor(const SolDoc *d, size_t target_line, size_t target_col)
{
    if (!d || !d->text) return 0;
    size_t len = sol_text_buffer_length(d->text);
    char buf[4096];
    size_t pos = 0, line = 0, line_start = 0;
    while (pos < len) {
        size_t got = sol_text_buffer_read(d->text, pos, buf, sizeof(buf));
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            if (line == target_line) {
                size_t col = (pos + i) - line_start;
                if (col == target_col || buf[i] == '\n') return pos + i;
            }
            if (buf[i] == '\n') {
                if (line == target_line) return pos + i;
                line++;
                line_start = pos + i + 1;
            }
        }
        pos += got;
    }
    return len;
}

static size_t line_length(const SolDoc *d, size_t line_idx)
{
    if (!d || !d->text) return 0;
    size_t len = sol_text_buffer_length(d->text);
    char buf[4096];
    size_t pos = 0, line = 0, line_start = 0;
    while (pos < len) {
        size_t got = sol_text_buffer_read(d->text, pos, buf, sizeof(buf));
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            if (buf[i] == '\n') {
                if (line == line_idx) return (pos + i) - line_start;
                line++;
                line_start = pos + i + 1;
            }
        }
        pos += got;
    }
    if (line == line_idx) return len - line_start;
    return 0;
}

static void move_cursor(SolDoc *d, int dx, int dy)
{
    if (!d) return;
    if (dx < 0) d->cursor = prev_char(d, d->cursor);
    if (dx > 0) d->cursor = next_char(d, d->cursor);
    if (dy != 0) {
        size_t line, col; cursor_to_lc(d, d->cursor, &line, &col);
        long target = (long)line + dy;
        if (target < 0) target = 0;
        size_t llen = line_length(d, (size_t)target);
        if (col > llen) col = llen;
        d->cursor = lc_to_cursor(d, (size_t)target, col);
    }
}

/* ============================================================
   Input dispatch
   ============================================================ */

bool sol_editor_input(SolEditor *ed, const SolInputEvent *ev)
{
    if (!ed || !ev) return false;

    /* Cancel pending chord on Esc. */
    if (ev->kind == SOL_INPUT_KEY_DOWN && ev->key == SOL_KEY_ESCAPE) {
        if (ed->flow_len > 0) { flow_reset(ed); return true; }
    }

    /* Leader-key chord: Cmd (Super) on macOS, Ctrl elsewhere. We accept
       either bit, plus optional Shift (we always uppercase the letter). */
    bool leader = (ev->mods & (SOL_MOD_SUPER | SOL_MOD_CTRL)) != 0;

    if (ev->kind == SOL_INPUT_KEY_DOWN && leader) {
        uint32_t k = ev->key;
        if (k >= 'A' && k <= 'Z')      return flow_step(ed, (char)k);
        if (k >= 'a' && k <= 'z')      return flow_step(ed, (char)(k - 32));
        /* Any other modified key clears any in-progress chord. */
        if (ed->flow_len > 0) flow_reset(ed);
        return false;
    }

    /* If a chord is mid-flight without leader held (which can happen
       briefly while the popup is visible), let plain text input proceed
       once the user starts typing again. We only consume keys while a
       chord is actively building. */

    SolDoc *d = sol_editor_doc_mut(ed, ed->active);
    if (!d) return false;

    if (ev->kind == SOL_INPUT_KEY_DOWN) {
        switch (ev->key) {
        case SOL_KEY_LEFT:  move_cursor(d, -1, 0); return true;
        case SOL_KEY_RIGHT: move_cursor(d, +1, 0); return true;
        case SOL_KEY_UP:    move_cursor(d, 0, -1); return true;
        case SOL_KEY_DOWN:  move_cursor(d, 0, +1); return true;
        case SOL_KEY_BACKSPACE:
            if (d->cursor > 0) {
                size_t prev = prev_char(d, d->cursor);
                sol_text_buffer_erase(d->text, prev, d->cursor - prev);
                d->cursor = prev;
            }
            return true;
        case SOL_KEY_DELETE: {
            size_t next = next_char(d, d->cursor);
            if (next > d->cursor)
                sol_text_buffer_erase(d->text, d->cursor, next - d->cursor);
            return true;
        }
        case SOL_KEY_ENTER:
            sol_text_buffer_insert(d->text, d->cursor, "\n", 1);
            d->cursor += 1;
            return true;
        case SOL_KEY_TAB:
            sol_text_buffer_insert(d->text, d->cursor, "    ", 4);
            d->cursor += 4;
            return true;
        default: break;
        }
        return false;
    }

    if (ev->kind == SOL_INPUT_TEXT) {
        if (ev->codepoint < 0x20) return false;
        char tmp[4]; size_t n;
        uint32_t cp = ev->codepoint;
        if (cp < 0x80)         { tmp[0] = (char)cp; n = 1; }
        else if (cp < 0x800)   { tmp[0] = (char)(0xC0 | (cp >> 6));
                                 tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12));
                                 tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                 tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
        else                   { tmp[0] = (char)(0xF0 | (cp >> 18));
                                 tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                 tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                 tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
        sol_text_buffer_insert(d->text, d->cursor, tmp, n);
        d->cursor += n;
        return true;
    }
    return false;
}

/* ============================================================
   Render
   ============================================================ */

static void render_tabs(SolEditor *ed)
{
    ca_div_begin(&(Ca_DivDesc){ .style = "tabs", .direction = 0 });
    for (int i = 0; i < SOL_EDITOR_MAX_DOCS; ++i) {
        SolDoc *d = &ed->docs[i];
        if (!d->in_use) continue;
        char label[80];
        snprintf(label, sizeof(label), "%s%s",
                 sol_text_buffer_dirty(d->text) ? "● " : "",
                 d->display_name);
        ca_text(&(Ca_TextDesc){
            .text  = label,
            .style = (i == ed->active) ? "tab active" : "tab",
        });
    }
    ca_div_end();
}

static void render_doc_view(SolEditor *ed, SolDoc *d)
{
    (void)ed;
    static size_t starts[MAX_VISIBLE_LINES + 64];
    size_t n_starts  = collect_line_starts(d, starts, MAX_VISIBLE_LINES + 64);
    size_t total_len = sol_text_buffer_length(d->text);

    size_t first = (d->scroll_line < n_starts) ? d->scroll_line : 0;
    size_t last  = first + MAX_VISIBLE_LINES;
    if (last > n_starts) last = n_starts;

    size_t cur_line, cur_col;
    cursor_to_lc(d, d->cursor, &cur_line, &cur_col);

    ca_div_begin(&(Ca_DivDesc){ .style = "editor-pane", .direction = 1 });

    char linebuf[2048];
    char numbuf[16];
    for (size_t li = first; li < last; ++li) {
        size_t start = starts[li];
        size_t end   = (li + 1 < n_starts) ? starts[li + 1] - 1 : total_len;
        if (end < start) end = start;
        size_t take = end - start;
        if (take > sizeof(linebuf) - 1) take = sizeof(linebuf) - 1;
        size_t got = sol_text_buffer_read(d->text, start, linebuf, take);
        linebuf[got] = '\0';

        bool is_cursor_line = (li == cur_line);

        ca_div_begin(&(Ca_DivDesc){
            .style = is_cursor_line ? "editor-line current" : "editor-line",
            .direction = 0,
        });
            snprintf(numbuf, sizeof(numbuf), "%4zu", li + 1);
            ca_text(&(Ca_TextDesc){ .text = numbuf, .style = "line-number" });

            if (is_cursor_line) {
                size_t cc = cur_col;
                if (cc > got) cc = got;
                char before[1024], cell[8] = " ", after[1024];
                size_t bn = (cc < sizeof(before)) ? cc : sizeof(before) - 1;
                memcpy(before, linebuf, bn); before[bn] = '\0';
                if (cc < got) {
                    cell[0] = (linebuf[cc] == '\t') ? ' ' : linebuf[cc];
                    cell[1] = '\0';
                    size_t an = got - cc - 1;
                    if (an > sizeof(after) - 1) an = sizeof(after) - 1;
                    memcpy(after, linebuf + cc + 1, an); after[an] = '\0';
                } else after[0] = '\0';
                ca_text(&(Ca_TextDesc){ .text = before, .style = "line-text" });
                ca_text(&(Ca_TextDesc){ .text = cell,   .style = "cursor-cell" });
                ca_text(&(Ca_TextDesc){ .text = after,  .style = "line-text" });
            } else {
                ca_text(&(Ca_TextDesc){ .text = linebuf, .style = "line-text" });
            }
        ca_div_end();
    }
    ca_div_end();
}

static void render_status(SolEditor *ed)
{
    char status[256];
    SolDoc *d = sol_editor_doc_mut(ed, ed->active);
    if (d) {
        size_t line, col; cursor_to_lc(d, d->cursor, &line, &col);
        snprintf(status, sizeof(status), "  %s%s   %zu:%zu",
                 d->display_name,
                 sol_text_buffer_dirty(d->text) ? " [+]" : "",
                 line + 1, col + 1);
    } else {
        snprintf(status, sizeof(status), "  (no document)");
    }

    ca_div_begin(&(Ca_DivDesc){ .style = "status-bar", .direction = 0 });
        ca_text(&(Ca_TextDesc){ .text = status, .style = "status-text" });
        if (ed->flow_len > 0) {
            char chord[64];
            snprintf(chord, sizeof(chord), "  ⌘ %s…", ed->flow_prefix);
            ca_text(&(Ca_TextDesc){ .text = chord, .style = "status-chord" });
        }
    ca_div_end();
}

/* Which-key popup: rows of "X  Label" for every entry that extends the
   current prefix by exactly one character. */
static void render_flow_popup(SolEditor *ed)
{
    if (ed->flow_len == 0) return;

    /* Title: chain of labels for ancestors. */
    char title[128];
    {
        char tmp[128] = {0};
        char acc[SOL_EDITOR_FLOW_MAX + 1] = {0};
        for (size_t i = 0; i < ed->flow_len; ++i) {
            acc[i] = ed->flow_prefix[i];
            acc[i + 1] = '\0';
            const SolFlowEntry *e = flow_lookup_exact(acc);
            if (i > 0) strncat(tmp, " › ", sizeof(tmp) - strlen(tmp) - 1);
            strncat(tmp, e ? e->label : acc, sizeof(tmp) - strlen(tmp) - 1);
        }
        snprintf(title, sizeof(title), "%s", tmp);
    }

    ca_div_begin(&(Ca_DivDesc){
        .style    = "flow-popup",
        .position = CA_POSITION_FIXED,
        .pos_x    = 24, .pos_y = 24,
        .width    = 320,
        .direction = 1,
    });
        ca_text(&(Ca_TextDesc){ .text = title, .style = "flow-title" });

        size_t plen = ed->flow_len;
        for (size_t i = 0; i < g_flow_count; ++i) {
            const SolFlowEntry *e = &g_flow[i];
            size_t elen = strlen(e->path);
            if (elen != plen + 1) continue;
            if (strncmp(e->path, ed->flow_prefix, plen) != 0) continue;

            char row[160];
            snprintf(row, sizeof(row), "  %c   %s%s",
                     e->path[elen - 1], e->label,
                     e->action ? "" : " ›");
            ca_text(&(Ca_TextDesc){ .text = row, .style = "flow-row" });
        }
    ca_div_end();
}

void sol_editor_render(SolEditor *ed, Ca_Window *win)
{
    (void)win;
    if (!ed) return;

    render_tabs(ed);

    SolDoc *d = sol_editor_doc_mut(ed, ed->active);
    if (d) render_doc_view(ed, d);
    else {
        ca_div_begin(&(Ca_DivDesc){ .style = "editor-pane empty" });
            ca_text(&(Ca_TextDesc){
                .text  = "No document. Hold ⌘ then F for File menu, or N for new scratch.",
                .style = "empty-hint" });
        ca_div_end();
    }

    render_status(ed);
    render_flow_popup(ed);
}
