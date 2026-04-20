#include "sol_textarea.h"
#include "sol_buffer.h"
#include "sol_event.h"

#include <causality.h>
#include <ca_theme.h>

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   CONSTANTS
   ============================================================ */

/* Approximate metrics for Roboto Mono Nerd Font at 14 px.
   These affect only the cursor pixel position within a line;
   all other layout is handled by causality's text renderer. */
#define LINE_HEIGHT   20.0f
#define CHAR_WIDTH     8.4f
#define GUTTER_WIDTH  44.0f   /* enough for 4 digits + a space gap */
#define SCROLL_SPEED  30.0f   /* must mirror causality's internal SCROLL_SPEED */

/* ============================================================
   GLFW KEY CONSTANTS
   ============================================================
   Causality forwards raw GLFW key codes through Ca_Event.key.key.
   We mirror only the subset we actually use here to avoid a direct
   GLFW header dependency.
   ============================================================ */

#define SOL_KEY_ESCAPE     256
#define SOL_KEY_ENTER      257
#define SOL_KEY_TAB        258
#define SOL_KEY_BACKSPACE  259
#define SOL_KEY_DELETE     261
#define SOL_KEY_RIGHT      262
#define SOL_KEY_LEFT       263
#define SOL_KEY_DOWN       264
#define SOL_KEY_UP         265
#define SOL_KEY_PAGE_UP    266
#define SOL_KEY_PAGE_DOWN  267
#define SOL_KEY_HOME       268
#define SOL_KEY_END        269
#define SOL_KEY_KP_ENTER   335

/* GLFW modifier bit-flags. */
#define SOL_MOD_SHIFT   0x0001
#define SOL_MOD_CONTROL 0x0002
#define SOL_MOD_ALT     0x0004
#define SOL_MOD_SUPER   0x0008

/* ============================================================
   INTERNAL STRUCT
   ============================================================ */

struct Sol_Textarea {
    Sol_Buffer *buf;

    /* Pixel dimensions of the visible editing area. */
    float width;
    float height;

    /* Vertical scroll offset in pixels (0 = top). */
    float scroll_y;

    /* Whether this textarea currently receives keyboard input. */
    bool focused;

    /* Causality node handle for the clipping outer container.
       Created in sol_textarea_build(); cleared each frame in sol_textarea_update(). */
    Ca_Div *outer_div;

    /* Event listener IDs for deregistration on destroy. */
    Sol_ListenerID scroll_lid;
    Sol_ListenerID key_lid;
    Sol_ListenerID char_lid;
    Sol_ListenerID resize_lid;
    Sol_ListenerID buf_change_lid;
    Sol_ListenerID cursor_lid;

    /* Set to true whenever something visual changed; cleared after rebuild.
       ca_div_clear is called ONLY when this is true, so causality marks
       nodes dirty only on real changes — not on every mouse-move or idle tick. */
    bool needs_rebuild;
};

/* ============================================================
   UTF-8 HELPERS
   ============================================================ */

/* Encode a Unicode codepoint as UTF-8.  Returns the byte count (1-4). */
static uint32_t codepoint_to_utf8(uint32_t cp, char out[4])
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

/* Number of bytes in the UTF-8 sequence starting with `first_byte`. */
static uint32_t utf8_seq_len(unsigned char c)
{
    if ((c & 0x80u) == 0x00u) return 1u;
    if ((c & 0xE0u) == 0xC0u) return 2u;
    if ((c & 0xF0u) == 0xE0u) return 3u;
    if ((c & 0xF8u) == 0xF0u) return 4u;
    return 1u;  /* invalid byte — treat as single */
}

/* Length of the UTF-8 sequence that ENDS at `offset` (exclusive). */
static uint32_t utf8_prev_seq_len(const char *data, uint32_t offset)
{
    if (offset == 0u) return 0u;
    uint32_t n = 1u;
    while (n < offset && n < 4u && (((unsigned char)data[offset - n]) & 0xC0u) == 0x80u)
        n++;
    return n;
}

/* ============================================================
   SCROLL HELPERS
   ============================================================ */

static void clamp_scroll(Sol_Textarea *tv)
{
    float total = (float)sol_buffer_line_count(tv->buf) * LINE_HEIGHT;
    float max_s = total - tv->height;
    if (max_s < 0.0f) max_s = 0.0f;
    if (tv->scroll_y < 0.0f)   tv->scroll_y = 0.0f;
    if (tv->scroll_y > max_s)  tv->scroll_y = max_s;
}

void sol_textarea_scroll_to_cursor(Sol_Textarea *tv)
{
    uint32_t cline, ccol;
    sol_buffer_cursor_get(tv->buf, &cline, &ccol);
    (void)ccol;

    float line_top    = (float)cline * LINE_HEIGHT;
    float line_bottom = line_top + LINE_HEIGHT;

    if (line_top < tv->scroll_y)
        tv->scroll_y = line_top;
    else if (line_bottom > tv->scroll_y + tv->height)
        tv->scroll_y = line_bottom - tv->height;

    clamp_scroll(tv);
}

/* ============================================================
   EVENT HANDLERS
   ============================================================ */

static void on_scroll(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    tv->scroll_y -= (float)ev->mouse_scroll.dy * SCROLL_SPEED;
    clamp_scroll(tv);
    tv->needs_rebuild = true;
}

static void on_resize(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    tv->width  = (float)ev->resize.width;
    tv->height = (float)ev->resize.height;
    clamp_scroll(tv);
    tv->needs_rebuild = true;
}

/* Forward declarations — defined below in the key handler section. */
static void on_key(Sol_Event *ev, void *user_data);
static void on_char(Sol_Event *ev, void *user_data);
static void clamp_cursor_to_line(Sol_Buffer *buf, uint32_t line, uint32_t *col);

/* Triggered by sol_buffer_insert / sol_buffer_delete via the event bus. */
static void on_buf_change(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    if (ev->change.buffer_id == sol_buffer_id(tv->buf))
        tv->needs_rebuild = true;
}

/* Triggered by sol_buffer_cursor_set / sol_buffer_cursor_move. */
static void on_cursor_move(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    if (ev->cursor.buffer_id == sol_buffer_id(tv->buf))
        tv->needs_rebuild = true;
}

/* ============================================================
   LIFECYCLE
   ============================================================ */

Sol_Textarea *sol_textarea_create(Sol_Buffer *buf,
                                   float init_width, float init_height)
{
    assert(buf);

    Sol_Textarea *tv = calloc(1, sizeof(Sol_Textarea));
    assert(tv && "sol: textarea allocation failed");

    tv->buf    = buf;
    tv->width  = init_width;
    tv->height = init_height;

    tv->scroll_lid     = sol_event_on(SOL_EVENT_MOUSE_SCROLL,   on_scroll,     tv);
    tv->key_lid        = sol_event_on(SOL_EVENT_KEY,            on_key,        tv);
    tv->char_lid       = sol_event_on(SOL_EVENT_CHAR,           on_char,       tv);
    tv->resize_lid     = sol_event_on(SOL_EVENT_WINDOW_RESIZE,  on_resize,     tv);
    tv->buf_change_lid = sol_event_on(SOL_EVENT_BUFFER_CHANGE,  on_buf_change, tv);
    tv->cursor_lid     = sol_event_on(SOL_EVENT_CURSOR_MOVE,    on_cursor_move, tv);

    tv->needs_rebuild = true; /* first frame always needs a build */

    return tv;
}

void sol_textarea_destroy(Sol_Textarea *tv)
{
    if (!tv) return;
    sol_event_off(tv->scroll_lid);
    sol_event_off(tv->key_lid);
    sol_event_off(tv->char_lid);
    sol_event_off(tv->resize_lid);
    sol_event_off(tv->buf_change_lid);
    sol_event_off(tv->cursor_lid);
    free(tv);
}

/* ============================================================
   BUFFER BINDING
   ============================================================ */

void sol_textarea_set_buffer(Sol_Textarea *tv, Sol_Buffer *buf)
{
    assert(buf);
    tv->buf           = buf;
    tv->scroll_y      = 0.0f;
    tv->needs_rebuild = true;
}

Sol_Buffer *sol_textarea_buffer(const Sol_Textarea *tv) { return tv->buf; }

/* ============================================================
   SIZE
   ============================================================ */

void sol_textarea_set_size(Sol_Textarea *tv, float width, float height)
{
    tv->width  = width;
    tv->height = height;
    clamp_scroll(tv);
}

/* ============================================================
   FOCUS
   ============================================================ */

void sol_textarea_focus(Sol_Textarea *tv)  { tv->focused = true;  tv->needs_rebuild = true; }
void sol_textarea_blur(Sol_Textarea *tv)   { tv->focused = false; tv->needs_rebuild = true; }
bool sol_textarea_is_focused(const Sol_Textarea *tv) { return tv->focused; }

/* ============================================================
   CAUSALITY UI BUILD  (called once inside ca_ui_begin/end)
   ============================================================ */

void sol_textarea_build(Sol_Textarea *tv, float width, float height)
{
    /* The outer div is the clipping container.
       width/height = 0 means "fill parent" in causality. */
    tv->outer_div = ca_div_begin(&(Ca_DivDesc){
        .width      = width,
        .height     = height,
        .style      = "sol-clip",
        .background = CA_THEME_BG_BASE,
    });

    /* Leave empty for now — sol_textarea_update fills it every frame. */
    ca_div_end();
}

/* ============================================================
   PER-FRAME UPDATE  (called inside ca_window_set_on_frame callback)
   ============================================================ */

void sol_textarea_update(Sol_Textarea *tv)
{
    if (!tv->outer_div) return;

    /* Nothing changed — skip the rebuild entirely so causality never sees
       dirty nodes on idle frames (mouse-move, etc.). */
    if (!tv->needs_rebuild) return;
    tv->needs_rebuild = false;

    Sol_Buffer *buf = tv->buf;

    clamp_scroll(tv);

    uint32_t num_lines = sol_buffer_line_count(buf);
    uint32_t cursor_line, cursor_col;
    sol_buffer_cursor_get(buf, &cursor_line, &cursor_col);
    Sol_Mode mode = sol_buffer_mode(buf);

    /* Compute the virtual window: which lines are visible. */
    uint32_t first = (uint32_t)(tv->scroll_y / LINE_HEIGHT);
    if (first >= num_lines) first = num_lines > 0u ? num_lines - 1u : 0u;

    uint32_t visible_count = (uint32_t)(tv->height / LINE_HEIGHT) + 2u;
    uint32_t last = first + visible_count;
    if (last > num_lines) last = num_lines;

    /* pos_y for the content div so that `first` appears at the viewport top
       with correct sub-line smooth-scroll offsets. */
    float content_pos_y = (float)first * LINE_HEIGHT - tv->scroll_y;

    /* ---- Rebuild the outer div's children ---- */
    ca_div_clear(tv->outer_div);

    /* Content div: absolutely positioned so virtual scrolling works.
       No explicit height → causality auto-sizes it to its children. */
    ca_div_begin(&(Ca_DivDesc){
        .direction  = CA_VERTICAL,
        .position   = CA_POSITION_ABSOLUTE,
        .pos_x      = 0.0f,
        .pos_y      = content_pos_y,
        .width      = tv->width > 0.0f ? tv->width : 0.0f,
    });

    /* Render visible lines. */
    for (uint32_t i = first; i < last; i++) {
        uint32_t len;
        const char *line_text = sol_buffer_line(buf, i, &len);
        if (!line_text) line_text = "";

        bool is_cursor_line = (i == cursor_line);

        /* ---- Row: gutter + content (+ cursor overlay) ---- */
        ca_div_begin(&(Ca_DivDesc){
            .direction  = CA_HORIZONTAL,
            .height     = LINE_HEIGHT,
            .background = is_cursor_line
                ? ca_color(0.11f, 0.11f, 0.16f, 1.0f)
                : CA_THEME_TRANSPARENT,
        });

        /* Gutter: line number */
        char gutter_buf[8];
        snprintf(gutter_buf, sizeof(gutter_buf), "%4u", i + 1u);
        ca_text(&(Ca_TextDesc){
            .text  = gutter_buf,
            .color = is_cursor_line ? CA_THEME_TEXT_MUTED : CA_THEME_TEXT_DIM,
            .width = GUTTER_WIDTH,
        });

        /* Content text */
        ca_text(&(Ca_TextDesc){
            .text  = line_text,
            .color = CA_THEME_TEXT_BRIGHT,
        });

        /* Cursor bar — only drawn when this textarea is focused AND this is
           the cursor line.  Absolutely positioned relative to the row div. */
        if (is_cursor_line && tv->focused) {
            float cursor_px = GUTTER_WIDTH + (float)cursor_col * CHAR_WIDTH;

            /* INSERT mode: thin I-beam.  NORMAL mode: full block (2× width). */
            float cursor_w = (mode == SOL_MODE_INSERT) ? 2.0f : CHAR_WIDTH;

            ca_div_begin(&(Ca_DivDesc){
                .position   = CA_POSITION_ABSOLUTE,
                .pos_x      = cursor_px,
                .pos_y      = 0.0f,
                .width      = cursor_w,
                .height     = LINE_HEIGHT,
                .background = CA_THEME_ACCENT,
                .z_index    = 1,
            });
            ca_div_end();
        }

        ca_div_end(); /* row */
    }

    ca_div_end(); /* content div */

    /* ---- Status bar at the bottom ---- */
    {
        const char *mode_str =
            (mode == SOL_MODE_INSERT) ? "INSERT" :
            (mode == SOL_MODE_VISUAL) ? "VISUAL" : "NORMAL";

        const char *path = sol_buffer_path(buf);
        const char *name = path ? path : "[No Name]";

        bool dirty = sol_buffer_is_dirty(buf);

        char status[256];
        snprintf(status, sizeof(status), " %s  %s%s  %u:%u ",
                 mode_str, name, dirty ? " [+]" : "",
                 cursor_line + 1u, cursor_col + 1u);

        /* Absolutely position the status bar at the bottom of the outer div. */
        ca_div_begin(&(Ca_DivDesc){
            .position   = CA_POSITION_ABSOLUTE,
            .pos_x      = 0.0f,
            .pos_y      = tv->height > LINE_HEIGHT ? tv->height - LINE_HEIGHT : 0.0f,
            .width      = tv->width > 0.0f ? tv->width : 0.0f,
            .height     = LINE_HEIGHT,
            .background = CA_THEME_ACCENT,
            .z_index    = 2,
        });
        ca_text(&(Ca_TextDesc){
            .text  = status,
            .color = ca_color(0.0f, 0.0f, 0.0f, 1.0f),
        });
        ca_div_end(); /* status bar */
    }

    ca_div_end(); /* outer_div (from ca_div_clear) */
}

/* ============================================================
   KEY INPUT HANDLER
   ============================================================ */

static void on_key(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    if (!tv->focused) return;
    if (ev->key.action != CA_PRESS && ev->key.action != CA_REPEAT) return;

    Sol_Buffer *buf = tv->buf;
    int key         = ev->key.key;
    int mods        = ev->key.mods;
    (void)mods;

    Sol_Mode mode = sol_buffer_mode(buf);

    uint32_t cl, cc;
    sol_buffer_cursor_get(buf, &cl, &cc);

    /* ---- Keys valid in ALL modes ---- */
    switch (key) {
    case SOL_KEY_UP:
        sol_buffer_cursor_move(buf, -1, 0);
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;

    case SOL_KEY_DOWN:
        sol_buffer_cursor_move(buf, +1, 0);
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;

    case SOL_KEY_LEFT:
        if (cc > 0u) {
            /* Move back one UTF-8 character. */
            uint32_t line_len;
            const char *ldata = sol_buffer_line(buf, cl, &line_len);
            uint32_t step = utf8_prev_seq_len(ldata, cc);
            sol_buffer_cursor_set(buf, cl, cc > step ? cc - step : 0u);
        } else if (cl > 0u) {
            /* Wrap to end of previous line. */
            uint32_t prev_len;
            sol_buffer_line(buf, cl - 1u, &prev_len);
            sol_buffer_cursor_set(buf, cl - 1u, prev_len);
        }
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;

    case SOL_KEY_RIGHT: {
        uint32_t line_len;
        const char *ldata = sol_buffer_line(buf, cl, &line_len);
        if (cc < line_len) {
            uint32_t step = utf8_seq_len((unsigned char)ldata[cc]);
            sol_buffer_cursor_set(buf, cl, cc + step);
        } else if (cl + 1u < sol_buffer_line_count(buf)) {
            /* Wrap to start of next line. */
            sol_buffer_cursor_set(buf, cl + 1u, 0u);
        }
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;
    }

    case SOL_KEY_HOME:
        sol_buffer_cursor_set(buf, cl, 0u);
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;

    case SOL_KEY_END: {
        uint32_t line_len;
        sol_buffer_line(buf, cl, &line_len);
        sol_buffer_cursor_set(buf, cl, line_len);
        sol_textarea_scroll_to_cursor(tv);
        ev->consumed = true;
        return;
    }

    case SOL_KEY_PAGE_UP: {
        uint32_t lines_per_page = (uint32_t)(tv->height / LINE_HEIGHT);
        tv->scroll_y -= (float)lines_per_page * LINE_HEIGHT;
        clamp_scroll(tv);
        int32_t new_line = (int32_t)cl - (int32_t)lines_per_page;
        if (new_line < 0) new_line = 0;
        sol_buffer_cursor_set(buf, (uint32_t)new_line, cc);
        ev->consumed = true;
        return;
    }

    case SOL_KEY_PAGE_DOWN: {
        uint32_t lines_per_page = (uint32_t)(tv->height / LINE_HEIGHT);
        tv->scroll_y += (float)lines_per_page * LINE_HEIGHT;
        clamp_scroll(tv);
        uint32_t new_line = cl + lines_per_page;
        uint32_t total = sol_buffer_line_count(buf);
        if (new_line >= total) new_line = total > 0u ? total - 1u : 0u;
        sol_buffer_cursor_set(buf, new_line, cc);
        ev->consumed = true;
        return;
    }

    default:
        break;
    }

    /* ---- NORMAL mode keys ---- */
    if (mode == SOL_MODE_NORMAL) {
        switch (key) {
        case 73: /* i — insert before cursor */
            sol_buffer_set_mode(buf, SOL_MODE_INSERT);
            tv->needs_rebuild = true; /* mode change has no event */
            ev->consumed = true;
            return;

        case 65: /* a — insert after cursor */
            sol_buffer_set_mode(buf, SOL_MODE_INSERT);
            /* Advance one character unless at end of line. */
            {
                uint32_t line_len;
                sol_buffer_line(buf, cl, &line_len);
                if (cc < line_len) sol_buffer_cursor_move(buf, 0, 1);
            }
            tv->needs_rebuild = true;
            ev->consumed = true;
            return;

        case 79: { /* o — open new line below */
            uint32_t line_len;
            sol_buffer_line(buf, cl, &line_len);
            sol_buffer_cursor_set(buf, cl, line_len);
            sol_buffer_insert(buf, cl, line_len, "\n", 1u);
            sol_buffer_cursor_set(buf, cl + 1u, 0u);
            sol_buffer_set_mode(buf, SOL_MODE_INSERT);
            sol_textarea_scroll_to_cursor(tv);
            ev->consumed = true;
            return;
        }

        case 88: { /* x — delete character under cursor */
            uint32_t line_len;
            const char *ldata = sol_buffer_line(buf, cl, &line_len);
            if (cc < line_len) {
                uint32_t step = utf8_seq_len((unsigned char)ldata[cc]);
                sol_buffer_delete(buf, cl, cc, cl, cc + step);
                clamp_cursor_to_line(buf, cl, &cc);
                sol_buffer_cursor_set(buf, cl, cc);
            }
            ev->consumed = true;
            return;
        }

        case SOL_KEY_ESCAPE:
            sol_buffer_selection_clear(buf);
            ev->consumed = true;
            return;

        default:
            break;
        }
        return; /* unknown normal-mode key — ignore */
    }

    /* ---- INSERT mode keys ---- */
    if (mode == SOL_MODE_INSERT) {
        switch (key) {
        case SOL_KEY_ESCAPE:
            sol_buffer_set_mode(buf, SOL_MODE_NORMAL);
            tv->needs_rebuild = true; /* mode change has no event */
            ev->consumed = true;
            return;

        case SOL_KEY_ENTER:
        case SOL_KEY_KP_ENTER:
            sol_buffer_insert(buf, cl, cc, "\n", 1u);
            sol_buffer_cursor_set(buf, cl + 1u, 0u);
            sol_textarea_scroll_to_cursor(tv);
            ev->consumed = true;
            return;

        case SOL_KEY_BACKSPACE:
            if (cc > 0u) {
                uint32_t line_len;
                const char *ldata = sol_buffer_line(buf, cl, &line_len);
                uint32_t step = utf8_prev_seq_len(ldata, cc);
                sol_buffer_delete(buf, cl, cc - step, cl, cc);
                sol_buffer_cursor_set(buf, cl, cc - step);
            } else if (cl > 0u) {
                /* Merge with previous line. */
                uint32_t prev_len;
                sol_buffer_line(buf, cl - 1u, &prev_len);
                sol_buffer_delete(buf, cl - 1u, prev_len, cl, 0u);
                sol_buffer_cursor_set(buf, cl - 1u, prev_len);
            }
            sol_textarea_scroll_to_cursor(tv);
            ev->consumed = true;
            return;

        case SOL_KEY_DELETE: {
            uint32_t line_len;
            const char *ldata = sol_buffer_line(buf, cl, &line_len);
            if (cc < line_len) {
                uint32_t step = utf8_seq_len((unsigned char)ldata[cc]);
                sol_buffer_delete(buf, cl, cc, cl, cc + step);
            } else if (cl + 1u < sol_buffer_line_count(buf)) {
                /* Merge next line. */
                sol_buffer_delete(buf, cl, line_len, cl + 1u, 0u);
            }
            ev->consumed = true;
            return;
        }

        case SOL_KEY_TAB:
            sol_buffer_insert(buf, cl, cc, "    ", 4u);
            sol_buffer_cursor_move(buf, 0, 4);
            ev->consumed = true;
            return;

        default:
            break;
        }
    }
}

/* Helper: clamp col to line length (called after deletes in normal mode). */
static void clamp_cursor_to_line(Sol_Buffer *buf, uint32_t line, uint32_t *col)
{
    uint32_t llen;
    sol_buffer_line(buf, line, &llen);
    if (*col > 0u && llen > 0u && *col >= llen)
        *col = llen - 1u;
    else if (llen == 0u)
        *col = 0u;
}

/* ============================================================
   CHARACTER INPUT HANDLER
   ============================================================ */

static void on_char(Sol_Event *ev, void *user_data)
{
    Sol_Textarea *tv = (Sol_Textarea *)user_data;
    if (!tv->focused) return;

    Sol_Buffer *buf = tv->buf;
    if (sol_buffer_mode(buf) != SOL_MODE_INSERT) return;

    /* Encode codepoint as UTF-8. */
    char utf8[4];
    uint32_t n = codepoint_to_utf8(ev->character.codepoint, utf8);

    uint32_t cl, cc;
    sol_buffer_cursor_get(buf, &cl, &cc);

    sol_buffer_insert(buf, cl, cc, utf8, n);
    sol_buffer_cursor_move(buf, 0, (int)n);
    sol_textarea_scroll_to_cursor(tv);

    ev->consumed = true;
}
