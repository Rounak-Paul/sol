#include <causality.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sol_buffer.h"
#include "sol_ui_constants.h"
#include "sol_system_manager.h"
#include "sol_ui_system.h"
#include "sol_file_picker.h"

typedef struct SolStartupPayload {
    uint32_t worker_count;
    uint32_t loaded_plugins;
    uint64_t warmup_checksum;
    bool input_binding_active;
} SolStartupPayload;

typedef struct SolWarmupContext {
    _Atomic uint64_t checksum;
} SolWarmupContext;

typedef struct SolAppContext {
    SolSystemManager *systems;
    SolEventBus *events;
    SolBufferSystem *buffers;
    SolJobSystem *jobs;
    SolInputSystem *input;
    SolSubscriptionToken startup_token;
    bool command_flows_ready;
    SolUISystem *ui;
    Ca_Instance *instance;
    /* Last-known cursor position in window CSS pixels; updated on every
       MOUSE_MOVE so MOUSE_SCROLL can route the wheel to the pane under
       the cursor (instead of the keyboard-focused one). */
    double mouse_x;
    double mouse_y;
} SolAppContext;

/* One editable line. Each line owns its own NUL-terminated growable
   byte buffer; `len` is strlen(data) and `cap` is the allocation size
   (always >= len + 1 to leave room for the terminator). Edits mutate
   a single line in place; Enter splits a line, Backspace at column 0
   joins with the previous line. */
typedef struct SolLine {
    char  *data;
    size_t len;
    size_t cap;
} SolLine;

typedef struct SolTextBufferState {
    SolLine *lines;
    size_t   line_count;
    size_t   line_capacity; /* allocation slots for the `lines` array */
    int      scroll_top;    /* index of the first visible line */
    int      cursor_line;   /* 0-based row of the caret */
    int      cursor_col;    /* 0-based BYTE column of the caret within the line */
    int      preferred_col; /* sticky column for vertical motion */
    char    *source_path;   /* absolute path on disk; NULL for unsaved/scratch */
} SolTextBufferState;

static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, value, len + 1u);
    return copy;
}

/* Slurp a whole file into a fresh, NUL-terminated heap buffer.
 * Returns NULL on any I/O error; *out_len is set on success. */
static char *sol_read_file_to_string(const char *path, size_t *out_len)
{
    if (!path) {
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long end = ftell(fp);
    if (end < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    size_t size = (size_t)end;
    char *buf = (char *)malloc(size + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t got = fread(buf, 1u, size, fp);
    fclose(fp);
    if (got != size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    if (out_len) {
        *out_len = size;
    }
    return buf;
}

static const char *sol_basename(const char *path)
{
    if (!path || !*path) {
        return "untitled";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sol_text_buffer_destroy(void *state)
{
    SolTextBufferState *text_state = (SolTextBufferState *)state;
    if (!text_state) {
        return;
    }

    free(text_state->source_path);
    if (text_state->lines) {
        for (size_t i = 0; i < text_state->line_count; ++i) {
            free(text_state->lines[i].data);
        }
        free(text_state->lines);
    }
    free(text_state);
}

/* ------------------------------------------------------------------ */
/* Line / state helpers                                                */
/* ------------------------------------------------------------------ */

/* Allocate a line with capacity for `min_cap` bytes (plus terminator).
   The minimum is rounded up to 16 bytes so single-character inserts
   into a fresh line don't reallocate every time. */
static bool sol_line_init(SolLine *line, const char *src, size_t len)
{
    size_t cap = len + 1u;
    if (cap < 16u) cap = 16u;
    line->data = (char *)malloc(cap);
    if (!line->data) return false;
    if (len > 0u && src) memcpy(line->data, src, len);
    line->data[len] = '\0';
    line->len = len;
    line->cap = cap;
    return true;
}

/* Ensure a line can hold `need` bytes plus terminator. Doubles up. */
static bool sol_line_reserve(SolLine *line, size_t need)
{
    size_t want = need + 1u;
    if (line->cap >= want) return true;
    size_t cap = line->cap ? line->cap : 16u;
    while (cap < want) cap *= 2u;
    char *grown = (char *)realloc(line->data, cap);
    if (!grown) return false;
    line->data = grown;
    line->cap  = cap;
    return true;
}

/* Insert `n` bytes at byte offset `at` within `line`. */
static bool sol_line_insert(SolLine *line, size_t at, const char *bytes, size_t n)
{
    if (at > line->len) at = line->len;
    if (!sol_line_reserve(line, line->len + n)) return false;
    memmove(line->data + at + n, line->data + at, line->len - at + 1u);
    if (n > 0u) memcpy(line->data + at, bytes, n);
    line->len += n;
    return true;
}

/* Erase `n` bytes at byte offset `at`. */
static void sol_line_erase(SolLine *line, size_t at, size_t n)
{
    if (at >= line->len) return;
    if (at + n > line->len) n = line->len - at;
    memmove(line->data + at, line->data + at + n, line->len - at - n + 1u);
    line->len -= n;
}

/* Grow the lines array to hold at least `need` entries. */
static bool sol_lines_reserve(SolTextBufferState *ts, size_t need)
{
    if (ts->line_capacity >= need) return true;
    size_t cap = ts->line_capacity ? ts->line_capacity : 8u;
    while (cap < need) cap *= 2u;
    SolLine *grown = (SolLine *)realloc(ts->lines, cap * sizeof(SolLine));
    if (!grown) return false;
    ts->lines = grown;
    ts->line_capacity = cap;
    return true;
}

/* Insert a fresh, zero-initialized line at index `at`. */
static bool sol_lines_insert(SolTextBufferState *ts, size_t at, const char *src, size_t len)
{
    if (at > ts->line_count) at = ts->line_count;
    if (!sol_lines_reserve(ts, ts->line_count + 1u)) return false;
    if (at < ts->line_count) {
        memmove(&ts->lines[at + 1u], &ts->lines[at],
                (ts->line_count - at) * sizeof(SolLine));
    }
    if (!sol_line_init(&ts->lines[at], src, len)) {
        /* Roll back the move so the array stays consistent. */
        if (at < ts->line_count) {
            memmove(&ts->lines[at], &ts->lines[at + 1u],
                    (ts->line_count - at) * sizeof(SolLine));
        }
        return false;
    }
    ts->line_count += 1u;
    return true;
}

/* Erase the line at index `at`, freeing its storage. */
static void sol_lines_erase(SolTextBufferState *ts, size_t at)
{
    if (at >= ts->line_count) return;
    free(ts->lines[at].data);
    if (at + 1u < ts->line_count) {
        memmove(&ts->lines[at], &ts->lines[at + 1u],
                (ts->line_count - at - 1u) * sizeof(SolLine));
    }
    ts->line_count -= 1u;
}

/* Build the initial line array from a single text blob. The blob is
   split on '\n'; the input bytes are NOT taken ownership of (each line
   is copied into its own allocation). On success the state owns
   everything and `text` may be freed by the caller. */
static bool sol_text_buffer_build_lines(SolTextBufferState *state, const char *text, size_t len)
{
    state->lines         = NULL;
    state->line_count    = 0u;
    state->line_capacity = 0u;
    state->scroll_top    = 0;
    state->cursor_line   = 0;
    state->cursor_col    = 0;
    state->preferred_col = 0;

    /* Always at least one line, even for an empty input. */
    size_t start = 0u;
    for (size_t i = 0; i <= len; ++i) {
        const bool at_eol = (i == len) || (text && text[i] == '\n');
        if (!at_eol) continue;
        const size_t line_len = i - start;
        if (!sol_lines_insert(state, state->line_count,
                              text ? text + start : NULL, line_len)) {
            return false;
        }
        start = i + 1u;
    }
    /* A trailing newline shouldn't produce a phantom blank line. */
    if (len > 0u && text && text[len - 1u] == '\n' && state->line_count > 1u) {
        sol_lines_erase(state, state->line_count - 1u);
    }
    if (state->line_count == 0u) {
        if (!sol_lines_insert(state, 0u, NULL, 0u)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                       */
/* ------------------------------------------------------------------ */

/* Encode a Unicode codepoint as UTF-8 into `out`. Returns the byte
   count (1..4), or 0 if the codepoint is invalid. */
static int sol_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0; /* surrogate */
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Distance to the next UTF-8 codepoint boundary at or after `at`. */
static size_t sol_utf8_next(const char *s, size_t len, size_t at)
{
    if (at >= len) return 0u;
    size_t i = at + 1u;
    while (i < len && (((unsigned char)s[i] & 0xC0u) == 0x80u)) i++;
    return i - at;
}

/* Distance to the previous UTF-8 codepoint boundary before `at`. */
static size_t sol_utf8_prev(const char *s, size_t at)
{
    if (at == 0u) return 0u;
    size_t i = at - 1u;
    while (i > 0u && (((unsigned char)s[i] & 0xC0u) == 0x80u)) i--;
    return at - i;
}

/* Fixed line geometry — must match `.buffer-line` height in style.h.
   Used to size the visible window and the custom scrollbar. */
#define SOL_TEXT_LINE_HEIGHT_PX 20

/* Estimate of UI chrome above/below the buffer pane in CSS pixels.
   Title bar (~30) + status bar (22) + tabs row (28) + buffer-text-col
   vertical padding (16) + a small fudge for the inter-strip borders.
   The result is only used to derive the visible-line count, so it
   doesn't have to be exact, but if it is too SMALL we'll under-render
   and leave empty space at the bottom of the pane. */
#define SOL_TEXT_PANE_CHROME_PX 100

static int sol_text_visible_lines(int window_h)
{
    int avail = window_h - SOL_TEXT_PANE_CHROME_PX;
    if (avail < SOL_TEXT_LINE_HEIGHT_PX) avail = SOL_TEXT_LINE_HEIGHT_PX;
    int n = avail / SOL_TEXT_LINE_HEIGHT_PX;
    /* Over-render by a couple of rows so the column always fully fills
       the parent pane even when our chrome estimate is slightly off.
       The parent has overflow:hidden so extra rows just clip. */
    n += 2;
    if (n < 1) n = 1;
    return n;
}

/* Click context emitted per visible line so causality can dispatch a
   line click back to us with enough state to reposition the cursor. */
typedef struct SolLineClickCtx {
    SolUISystem        *ui;
    SolBufferNodeId     leaf_id;
    SolTextBufferState *ts;
    int                 line_idx;
} SolLineClickCtx;

/* Per-frame ring of click contexts. Each rebuild reuses slots from the
   start of the ring; causality keeps a pointer into this array via the
   button's click_data, so the slot must outlive the frame. 1024 slots
   is overkill for any realistic viewport (we render ~50 rows per pane
   per frame). */
#define SOL_LINE_CLICK_RING 1024
static SolLineClickCtx g_sol_line_click_ring[SOL_LINE_CLICK_RING];
static int             g_sol_line_click_cursor = 0;

static SolLineClickCtx *sol_acquire_line_click_ctx(void)
{
    SolLineClickCtx *cb = &g_sol_line_click_ring[
        g_sol_line_click_cursor++ & (SOL_LINE_CLICK_RING - 1)];
    return cb;
}

static void sol_on_buffer_pointer(const Ca_DragEvent *ev, void *user_data)
{
    SolLineClickCtx *cb = (SolLineClickCtx *)user_data;
    if (!ev || !cb || !cb->ui || !cb->ts) return;

    /* Focus the host pane so subsequent typing lands here. */
    sol_ui_system_focus_leaf(cb->ui, cb->leaf_id);

    SolTextBufferState *ts = cb->ts;
    if (ts->line_count == 0u) return;

    /* The text-column CSS has 8 px padding all around. The first
       visible row's top edge sits at local_y == padding_top. Each
       row is SOL_TEXT_LINE_HEIGHT_PX tall. */
    const float pad_x = 8.0f;
    const float pad_y = 8.0f;
    float local_x = ev->local_x - pad_x;
    float local_y = ev->local_y - pad_y;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_y < 0.0f) local_y = 0.0f;

    int row = (int)(local_y / (float)SOL_TEXT_LINE_HEIGHT_PX);
    int line_idx = ts->scroll_top + row;
    if (line_idx < 0) line_idx = 0;
    if ((size_t)line_idx >= ts->line_count)
        line_idx = (int)ts->line_count - 1;
    ts->cursor_line = line_idx;

    const SolLine *line = &ts->lines[line_idx];

    Ca_Window *win = sol_ui_system_primary_window(cb->ui);
    float glyph_advance_px = win
        ? ca_measure_text_px(win, "M", SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)
        : 0.0f;
    if (glyph_advance_px <= 0.0f)
        glyph_advance_px = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;

    int target_cp = (int)((local_x / glyph_advance_px) + 0.5f);
    if (target_cp < 0) target_cp = 0;

    /* Walk codepoints to translate cp index into byte offset. */
    size_t at = 0u;
    int    cp = 0;
    while (cp < target_cp && at < line->len && line->data) {
        size_t step = sol_utf8_next(line->data, line->len, at);
        if (step == 0u) break;
        at += step;
        cp++;
    }

    ts->cursor_col    = (int)at;
    ts->preferred_col = at;
    sol_ui_system_invalidate_buffer_area(cb->ui);
}

static void sol_text_buffer_render(const SolBuffer *buffer, const SolBufferRenderArgs *args, void *state)
{
    (void)buffer;

    SolTextBufferState *ts = (SolTextBufferState *)state;
    if (!ts) {
        return;
    }

    int win_h = 0;
    if (args && args->ui_context) {
        sol_ui_system_window_size((const SolUISystem *)args->ui_context, NULL, &win_h);
    }
    if (win_h <= 0) win_h = 600;

    /* `rendered` is what we actually emit (slightly more than fits, so
       the parent always looks full). `viewport` is what the user can
       actually see and is used for scrollbar thumb math so the thumb
       has the correct length relative to the file. */
    const int rendered = sol_text_visible_lines(win_h);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;

    const int total   = (int)ts->line_count;
    int max_top = total > viewport ? total - viewport : 0;
    if (ts->scroll_top < 0)        ts->scroll_top = 0;
    if (ts->scroll_top > max_top)  ts->scroll_top = max_top;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "buffer-scroll-row",
    });

    /* --- Left gutter: line numbers ---------------------------------- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "buffer-gutter-col",
    });
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = ts->scroll_top + i;
        if (line_idx >= total) {
            ca_div_begin(&(Ca_DivDesc){ .style = "buffer-gutter-line-empty" });
            ca_div_end();
            continue;
        }
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%d", line_idx + 1);
        /* Causality copies the `text` pointer; we need stable storage
           per emission. Use a small per-frame static ring of strings
           so each row gets a unique buffer. */
        static char ring[128][16];
        static int  ring_cursor = 0;
        const int   slot = ring_cursor++ & 127;
        memcpy(ring[slot], num_buf, sizeof(num_buf));
        ca_text(&(Ca_TextDesc){
            .text  = ring[slot],
            .style = "buffer-gutter-line",
        });
    }
    ca_div_end();   /* buffer-gutter-col */

    /* --- Text column: one ca_text per visible line ----------------
       The text column itself owns the click/drag handler. A single
       on_drag_start fires on mouse-down with node-local coordinates;
       we convert (local_x, local_y) into (line, column) directly,
       just like a normal text editor. No per-line buttons, so clicks
       reliably hit the column regardless of nesting. */
    SolLineClickCtx *cb = sol_acquire_line_click_ctx();
    cb->ui       = args ? (SolUISystem *)args->ui_context : NULL;
    cb->leaf_id  = args ? args->leaf_id : 0u;
    cb->ts       = ts;
    cb->line_idx = 0;
    ca_div_begin(&(Ca_DivDesc){
        .direction     = CA_VERTICAL,
        .style         = "buffer-text-col",
        .on_drag_start = (cb->ui != NULL) ? sol_on_buffer_pointer : NULL,
        .drag_data     = cb,
    });
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = ts->scroll_top + i;
        if (line_idx >= total) {
            ca_div_begin(&(Ca_DivDesc){ .style = "buffer-line-empty" });
            ca_div_end();
            continue;
        }
        const SolLine *line = &ts->lines[line_idx];
        const bool is_cursor_line = (args && args->is_active &&
                                     line_idx == ts->cursor_line);

        /* Single row container per line. The active line additionally
           overlays an absolutely-positioned caret div (which is removed
           from flex flow) so cursor and non-cursor lines have IDENTICAL
           layout and text never shifts. */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "buffer-line-row",
        });

        ca_text(&(Ca_TextDesc){
            .text  = (line->data && line->len > 0u) ? line->data : " ",
            .style = "buffer-line",
        });

        if (is_cursor_line) {
            /* Compute caret pixel offset from the rendered font's
               actual monospace advance, NOT a hard-coded 0.6 ratio. */
            int col = ts->cursor_col;
            if (col < 0) col = 0;
            if ((size_t)col > line->len) col = (int)line->len;
            int cp_count = 0;
            size_t walk = 0;
            while (walk < (size_t)col && line->data) {
                size_t step = sol_utf8_next(line->data, line->len, walk);
                if (step == 0u) break;
                walk += step;
                cp_count++;
            }
            Ca_Window *win = (args && args->ui_context)
                ? sol_ui_system_primary_window((SolUISystem *)args->ui_context)
                : NULL;
            float glyph_advance_px = win
                ? ca_measure_text_px(win, "M", SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)
                : 0.0f;
            if (glyph_advance_px <= 0.0f)
                glyph_advance_px = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;
            const float caret_x = (float)cp_count * glyph_advance_px;
            ca_div_begin(&(Ca_DivDesc){
                .style    = "buffer-caret",
                .position = CA_POSITION_ABSOLUTE,
                .pos_x    = caret_x,
                .pos_y    = 1.0f,
            });
            ca_div_end();
        }

        ca_div_end();   /* buffer-line-row */
    }
    ca_div_end();   /* buffer-text-col */

    /* --- Custom scrollbar (only when the file overflows) ----------- */
    if (total > viewport && max_top > 0) {
        const float track_h     = (float)(viewport * SOL_TEXT_LINE_HEIGHT_PX);
        float thumb_h           = track_h * (float)viewport / (float)total;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        const float free_h      = track_h - thumb_h;
        const float top_spacer  = free_h * (float)ts->scroll_top / (float)max_top;

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "buffer-scrollbar",
        });
        /* Causality treats `.height == 0` as auto, which makes a flex
           child grow to fill — that would push the thumb to the bottom
           even when the user is at the top of the file. Skip the spacer
           entirely when its computed height is effectively zero. */
        if (top_spacer >= 0.5f) {
            ca_div_begin(&(Ca_DivDesc){
                .style  = "buffer-scrollbar-spacer",
                .height = top_spacer,
            });
            ca_div_end();
        }
        ca_div_begin(&(Ca_DivDesc){
            .style  = args && args->is_active
                          ? "buffer-scrollbar-thumb buffer-scrollbar-thumb-active"
                          : "buffer-scrollbar-thumb",
            .height = thumb_h,
        });
        ca_div_end();
        ca_div_end();   /* buffer-scrollbar */
    }

    ca_div_end();   /* buffer-scroll-row */
}

/* Split `text` (heap-owned) into lines: each output line gets its own
   allocation. The caller retains ownership of `text`. */

static SolBufferId sol_create_text_buffer(SolBufferSystem *buffers, const char *name, const char *text, const char *source_path)
{
    if (!buffers) {
        return 0u;
    }

    SolTextBufferState *state = (SolTextBufferState *)calloc(1u, sizeof(SolTextBufferState));
    if (!state) {
        return 0u;
    }

    /* `text` is borrowed; build_lines copies each line into its own
       heap-allocated growable buffer. */
    const size_t len = text ? strlen(text) : 0u;
    if (!sol_text_buffer_build_lines(state, text, len)) {
        sol_text_buffer_destroy(state);
        return 0u;
    }

    if (source_path) {
        state->source_path = sol_strdup(source_path);
        /* Allocation failure here is non-fatal — the buffer is still
           usable, just can't be matched for dedupe. */
    }

    SolBufferDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = name;
    desc.kind = SOL_BUFFER_KIND_TEXT;
    desc.state = state;
    desc.ops.destroy = sol_text_buffer_destroy;
    desc.ops.render = sol_text_buffer_render;

    const SolBufferId id = sol_buffer_create(buffers, &desc);
    if (id == 0u) {
        sol_text_buffer_destroy(state);
    }

    return id;
}

/* Find an existing text buffer whose on-disk source path matches.
   Returns 0 when no match. Used to dedupe file opens so clicking the
   same file in the tree (or via the picker) reuses the existing tab
   instead of creating a new one — VS Code / Neovim style. */
static SolBufferId sol_find_text_buffer_by_path(SolBufferSystem *buffers, const char *path)
{
    if (!buffers || !path) return 0u;
    const size_t total = sol_buffer_count(buffers);
    for (size_t i = 0u; i < total; ++i) {
        const SolBufferId id = sol_buffer_at(buffers, i);
        if (id == 0u) continue;
        SolBuffer *buf = sol_buffer_get(buffers, id);
        if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) continue;
        const SolTextBufferState *ts =
            (const SolTextBufferState *)sol_buffer_state(buf);
        if (!ts || !ts->source_path) continue;
        if (strcmp(ts->source_path, path) == 0) return id;
    }
    return 0u;
}

/* Open `path` as a text buffer focused in the active leaf. If a buffer
 * for this path already exists, reuse it instead of creating a new one.
 * Returns true on success. */
static bool sol_open_path_in_active_leaf(SolBufferSystem *buffers, const char *path)
{
    if (!buffers || !path) return false;

    const SolBufferId existing = sol_find_text_buffer_by_path(buffers, path);
    if (existing != 0u) {
        return sol_buffer_set_active_leaf_buffer(buffers, existing);
    }

    size_t len = 0u;
    char *contents = sol_read_file_to_string(path, &len);
    if (!contents) {
        fprintf(stderr, "sol: cannot open '%s'\n", path);
        return false;
    }

    SolBufferId id = sol_create_text_buffer(buffers, sol_basename(path), contents, path);
    free(contents);
    if (id == 0u) {
        fprintf(stderr, "sol: failed to create buffer for '%s'\n", path);
        return false;
    }
    if (!sol_buffer_set_active_leaf_buffer(buffers, id)) {
        fprintf(stderr, "sol: failed to focus buffer for '%s'\n", path);
    }
    return true;
}

/* Bridge: SolUIFileOpenFn signature → buffer system. */
static bool sol_on_tree_file_open(const char *path, void *user_data)
{
    SolBufferSystem *buffers = (SolBufferSystem *)user_data;
    return sol_open_path_in_active_leaf(buffers, path);
}

/* File-picker callbacks. These run from inside a click handler in the
 * picker window's UI; the picker handle reaps itself on the next tick. */
static void sol_on_picker_file_chosen(const char *path, void *user_data)
{
    if (!path) return;   /* user cancelled */
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app) return;
    if (sol_open_path_in_active_leaf(app->buffers, path) && app->ui) {
        /* Tell the buffer area to rebuild so the newly-opened file
           appears immediately instead of after some unrelated update. */
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_picker_folder_chosen(const char *path, void *user_data)
{
    if (!path) return;   /* user cancelled */
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->ui) return;
    if (!sol_ui_system_set_file_tree_root(app->ui, path)) {
        fprintf(stderr, "sol: cannot open directory '%s'\n", path);
    }
}

/* Title-bar menu trampolines: open a picker in the matching mode. */
static void sol_on_menu_open_file(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance,
                         SOL_FILE_PICKER_FILE,
                         NULL,
                         sol_on_picker_file_chosen,
                         app);
}

static void sol_on_menu_open_folder(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance,
                         SOL_FILE_PICKER_FOLDER,
                         NULL,
                         sol_on_picker_folder_chosen,
                         app);
}

static SolModifierMask sol_modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if ((mods & 0x0001) != 0) {
        out |= SOL_MOD_SHIFT;
    }
    if ((mods & 0x0002) != 0) {
        out |= SOL_MOD_CTRL;
    }
    if ((mods & 0x0004) != 0) {
        out |= SOL_MOD_ALT;
    }
    if ((mods & 0x0008) != 0) {
        out |= SOL_MOD_SUPER;
    }
    return out;
}

static bool sol_on_startup_event(const SolEvent *event, void *user_data)
{
    (void)user_data;

    if (!event || !event->payload || event->payload_size != sizeof(SolStartupPayload)) {
        return false;
    }

    const SolStartupPayload *payload = (const SolStartupPayload *)event->payload;
    printf(
        "[sol] startup: workers=%u plugins=%u warmup=%llu input=%s\n",
        payload->worker_count,
        payload->loaded_plugins,
        (unsigned long long)payload->warmup_checksum,
        payload->input_binding_active ? "ready" : "missing"
    );

    return false;
}

static void sol_warmup_range(uint32_t begin, uint32_t end, void *user_data)
{
    SolWarmupContext *context = (SolWarmupContext *)user_data;
    uint64_t local_sum = 0u;

    for (uint32_t i = begin; i < end; ++i) {
        local_sum += ((uint64_t)i * 2654435761ull) ^ ((uint64_t)i >> 3u);
    }

    atomic_fetch_add_explicit(&context->checksum, local_sum, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Active text-buffer access + editing primitives                      */
/* ------------------------------------------------------------------ */

/* Borrow the active text buffer's editable state, or NULL if there
   isn't one (no buffer focused, or focused buffer isn't text-kind). */
static SolTextBufferState *sol_active_text_state(SolBufferSystem *buffers)
{
    if (!buffers) return NULL;
    const SolBufferId id = sol_buffer_active_buffer(buffers);
    if (id == 0u) return NULL;
    SolBuffer *buf = sol_buffer_get(buffers, id);
    if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) return NULL;
    return (SolTextBufferState *)sol_buffer_state(buf);
}

/* Clamp the cursor to a valid (line, col) pair. The col is clamped to
   the line's byte length so it can never point past the terminator. */
static void sol_text_clamp_cursor(SolTextBufferState *ts)
{
    if (!ts || ts->line_count == 0u) return;
    if (ts->cursor_line < 0) ts->cursor_line = 0;
    if ((size_t)ts->cursor_line >= ts->line_count) {
        ts->cursor_line = (int)ts->line_count - 1;
    }
    const SolLine *line = &ts->lines[ts->cursor_line];
    if (ts->cursor_col < 0) ts->cursor_col = 0;
    if ((size_t)ts->cursor_col > line->len) ts->cursor_col = (int)line->len;
}

/* Adjust scroll_top so the cursor line is visible. `viewport` is the
   number of lines fully on-screen. */
static void sol_text_ensure_cursor_visible(SolTextBufferState *ts, int viewport)
{
    if (!ts || viewport <= 0) return;
    if (ts->cursor_line < ts->scroll_top) {
        ts->scroll_top = ts->cursor_line;
    } else if (ts->cursor_line >= ts->scroll_top + viewport) {
        ts->scroll_top = ts->cursor_line - viewport + 1;
    }
    if (ts->scroll_top < 0) ts->scroll_top = 0;
}

/* Insert a single Unicode codepoint at the cursor and advance.
   Returns true if the buffer was modified. */
static bool sol_text_insert_codepoint(SolTextBufferState *ts, uint32_t cp)
{
    if (!ts || ts->line_count == 0u) return false;
    char enc[4];
    int n = sol_utf8_encode(cp, enc);
    if (n <= 0) return false;
    sol_text_clamp_cursor(ts);
    SolLine *line = &ts->lines[ts->cursor_line];
    if (!sol_line_insert(line, (size_t)ts->cursor_col, enc, (size_t)n)) {
        return false;
    }
    ts->cursor_col += n;
    ts->preferred_col = ts->cursor_col;
    return true;
}

/* Split the current line at the cursor, creating a new line below. */
static bool sol_text_insert_newline(SolTextBufferState *ts)
{
    if (!ts || ts->line_count == 0u) return false;
    sol_text_clamp_cursor(ts);
    SolLine *line = &ts->lines[ts->cursor_line];
    const size_t tail_at  = (size_t)ts->cursor_col;
    const size_t tail_len = line->len - tail_at;
    /* Insert a fresh line below carrying the tail bytes. */
    if (!sol_lines_insert(ts, (size_t)ts->cursor_line + 1u,
                          line->data + tail_at, tail_len)) {
        return false;
    }
    /* Re-fetch — the array may have moved. */
    line = &ts->lines[ts->cursor_line];
    sol_line_erase(line, tail_at, tail_len);
    ts->cursor_line += 1;
    ts->cursor_col   = 0;
    ts->preferred_col = 0;
    return true;
}

/* Delete the codepoint immediately to the LEFT of the cursor. At
   column 0 of a non-first line, joins with the previous line. */
static bool sol_text_backspace(SolTextBufferState *ts)
{
    if (!ts || ts->line_count == 0u) return false;
    sol_text_clamp_cursor(ts);
    if (ts->cursor_col > 0) {
        SolLine *line = &ts->lines[ts->cursor_line];
        const size_t prev = sol_utf8_prev(line->data, (size_t)ts->cursor_col);
        if (prev == 0u) return false;
        sol_line_erase(line, (size_t)ts->cursor_col - prev, prev);
        ts->cursor_col -= (int)prev;
        ts->preferred_col = ts->cursor_col;
        return true;
    }
    if (ts->cursor_line == 0) return false;
    /* Join: append current line to previous; remove current. */
    SolLine *prev_line = &ts->lines[ts->cursor_line - 1];
    SolLine *cur_line  = &ts->lines[ts->cursor_line];
    const size_t join_col = prev_line->len;
    if (cur_line->len > 0u) {
        if (!sol_line_insert(prev_line, prev_line->len,
                             cur_line->data, cur_line->len)) {
            return false;
        }
    }
    sol_lines_erase(ts, (size_t)ts->cursor_line);
    ts->cursor_line -= 1;
    ts->cursor_col = (int)join_col;
    ts->preferred_col = ts->cursor_col;
    return true;
}

/* Delete the codepoint immediately to the RIGHT of the cursor. At
   end-of-line of a non-last line, joins the next line into this one. */
static bool sol_text_delete_forward(SolTextBufferState *ts)
{
    if (!ts || ts->line_count == 0u) return false;
    sol_text_clamp_cursor(ts);
    SolLine *line = &ts->lines[ts->cursor_line];
    if ((size_t)ts->cursor_col < line->len) {
        const size_t n = sol_utf8_next(line->data, line->len, (size_t)ts->cursor_col);
        if (n == 0u) return false;
        sol_line_erase(line, (size_t)ts->cursor_col, n);
        return true;
    }
    if ((size_t)ts->cursor_line + 1u >= ts->line_count) return false;
    SolLine *next_line = &ts->lines[ts->cursor_line + 1];
    if (next_line->len > 0u) {
        if (!sol_line_insert(line, line->len, next_line->data, next_line->len)) {
            return false;
        }
    }
    sol_lines_erase(ts, (size_t)ts->cursor_line + 1u);
    return true;
}

/* Cursor motion. dx/dy are signed deltas in codepoints / lines. */
static void sol_text_move_cursor(SolTextBufferState *ts, int dx, int dy, bool sticky_col)
{
    if (!ts || ts->line_count == 0u) return;
    sol_text_clamp_cursor(ts);

    if (dx != 0 && dy == 0) {
        if (dx > 0) {
            for (int i = 0; i < dx; ++i) {
                SolLine *line = &ts->lines[ts->cursor_line];
                if ((size_t)ts->cursor_col < line->len) {
                    size_t n = sol_utf8_next(line->data, line->len, (size_t)ts->cursor_col);
                    if (n == 0u) break;
                    ts->cursor_col += (int)n;
                } else if ((size_t)ts->cursor_line + 1u < ts->line_count) {
                    ts->cursor_line += 1;
                    ts->cursor_col = 0;
                } else {
                    break;
                }
            }
        } else {
            for (int i = 0; i < -dx; ++i) {
                if (ts->cursor_col > 0) {
                    SolLine *line = &ts->lines[ts->cursor_line];
                    size_t n = sol_utf8_prev(line->data, (size_t)ts->cursor_col);
                    if (n == 0u) break;
                    ts->cursor_col -= (int)n;
                } else if (ts->cursor_line > 0) {
                    ts->cursor_line -= 1;
                    ts->cursor_col = (int)ts->lines[ts->cursor_line].len;
                } else {
                    break;
                }
            }
        }
        ts->preferred_col = ts->cursor_col;
        return;
    }

    if (dy != 0) {
        const int target_col = sticky_col ? ts->preferred_col : ts->cursor_col;
        ts->cursor_line += dy;
        if (ts->cursor_line < 0) ts->cursor_line = 0;
        if ((size_t)ts->cursor_line >= ts->line_count) {
            ts->cursor_line = (int)ts->line_count - 1;
        }
        const SolLine *line = &ts->lines[ts->cursor_line];
        ts->cursor_col = target_col;
        if (ts->cursor_col < 0) ts->cursor_col = 0;
        if ((size_t)ts->cursor_col > line->len) ts->cursor_col = (int)line->len;
        /* Snap to a UTF-8 boundary if we landed inside a multibyte glyph. */
        while (ts->cursor_col > 0 &&
               ((unsigned char)line->data[ts->cursor_col] & 0xC0u) == 0x80u) {
            ts->cursor_col -= 1;
        }
    }
}

/* True when this key, on its own, is part of a printable codepoint that
   will arrive via the CHAR callback — so we DON'T want to also act on
   it from the key callback (else letters double-up). */
static bool sol_key_is_printable_alpha(SolKeyCode key)
{
    return (key >= 32 && key <= 126);
}

/* Handle a single non-leader key press for the active text buffer.
   Returns true if the key was consumed. */
static bool sol_handle_text_buffer_key(SolAppContext *app,
                                       SolKeyCode key, SolModifierMask mods)
{
    SolTextBufferState *ts = sol_active_text_state(app->buffers);
    if (!ts) return false;

    /* Editing keys that don't carry modifiers (Backspace, arrows, …)
       are dispatched here. Modifier-bearing chords belong to the flow
       system and are handled by sol_ui_system_handle_input_event before
       we get called. We only consume CTRL-less / SUPER-less variants. */
    const bool has_chord = (mods & (SOL_MOD_CTRL | SOL_MOD_SUPER | SOL_MOD_ALT)) != 0u;

    bool handled = false;
    switch (key) {
    case SOL_KEY_LEFT:      if (!has_chord) { sol_text_move_cursor(ts, -1,  0, false); handled = true; } break;
    case SOL_KEY_RIGHT:     if (!has_chord) { sol_text_move_cursor(ts, +1,  0, false); handled = true; } break;
    case SOL_KEY_UP:        if (!has_chord) { sol_text_move_cursor(ts,  0, -1, true);  handled = true; } break;
    case SOL_KEY_DOWN:      if (!has_chord) { sol_text_move_cursor(ts,  0, +1, true);  handled = true; } break;
    case SOL_KEY_HOME:      if (!has_chord) { ts->cursor_col = 0; ts->preferred_col = 0; handled = true; } break;
    case SOL_KEY_END:       if (!has_chord) {
        sol_text_clamp_cursor(ts);
        ts->cursor_col = (int)ts->lines[ts->cursor_line].len;
        ts->preferred_col = ts->cursor_col;
        handled = true;
    } break;
    case SOL_KEY_BACKSPACE: if (!has_chord) handled = sol_text_backspace(ts); break;
    case SOL_KEY_DELETE:    if (!has_chord) handled = sol_text_delete_forward(ts); break;
    case SOL_KEY_ENTER:     if (!has_chord) handled = sol_text_insert_newline(ts); break;
    default: break;
    }

    if (!handled) return false;

    /* Keep the cursor in view and rebuild. */
    int win_h = 0;
    sol_ui_system_window_size(app->ui, NULL, &win_h);
    if (win_h <= 0) win_h = 600;
    int viewport = sol_text_visible_lines(win_h) - 2;
    if (viewport < 1) viewport = 1;
    sol_text_ensure_cursor_visible(ts, viewport);
    sol_ui_system_invalidate_buffer_area(app->ui);
    return true;
}


static void sol_on_ca_key(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->key.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_KEY_UP;
    } else if (ev->key.action == CA_PRESS || ev->key.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_KEY_DOWN;
    } else {
        return;
    }

    input_event.data.key.key = (SolKeyCode)ev->key.key;
    input_event.data.key.modifiers = sol_modifiers_from_ca(ev->key.mods);
    input_event.data.key.repeated = (ev->key.action == CA_REPEAT);

    const bool ui_consumed =
        sol_ui_system_handle_input_event(app->ui, &input_event);
    sol_input_system_process_event(app->input, &input_event);

    /* Buffer keymap layer: only on KEY_DOWN / repeat, only when the
       leader popup isn't capturing input, and only when the UI didn't
       already act on this key. Printable letters/digits are skipped
       here — they arrive via the CHAR callback as decoded codepoints,
       which is the right path for IME / dead-key support. */
    if (input_event.type != SOL_INPUT_EVENT_KEY_DOWN) return;
    if (ui_consumed) return;
    if (sol_ui_system_is_leader_active(app->ui)) return;
    if (sol_key_is_printable_alpha(input_event.data.key.key)) return;
    sol_handle_text_buffer_key(app, input_event.data.key.key,
                               input_event.data.key.modifiers);
}

static void sol_on_ca_char(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_TEXT_INPUT;
    input_event.data.text.codepoint = ev->character.codepoint;

    sol_input_system_process_event(app->input, &input_event);

    /* Route the codepoint to the active text buffer's cursor when the
       leader chord isn't capturing input. Glyphs that should never be
       inserted (NUL, control chars below SPACE other than TAB) are
       filtered here so we don't smuggle them into the buffer. */
    if (sol_ui_system_is_leader_active(app->ui)) return;
    const uint32_t cp = ev->character.codepoint;
    if (cp == 0u) return;
    if (cp < 0x20u && cp != 0x09u) return;   /* skip C0 controls except TAB */
    if (cp == 0x7Fu) return;                  /* DEL — Backspace path handles delete */
    SolTextBufferState *ts = sol_active_text_state(app->buffers);
    if (!ts) return;
    if (sol_text_insert_codepoint(ts, cp)) {
        int win_h = 0;
        sol_ui_system_window_size(app->ui, NULL, &win_h);
        if (win_h <= 0) win_h = 600;
        int viewport = sol_text_visible_lines(win_h) - 2;
        if (viewport < 1) viewport = 1;
        sol_text_ensure_cursor_visible(ts, viewport);
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_ca_mouse_button(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->mouse_button.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_UP;
    } else if (ev->mouse_button.action == CA_PRESS || ev->mouse_button.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_DOWN;
    } else {
        return;
    }

    input_event.data.mouse_button.button = (SolMouseButton)ev->mouse_button.button;
    input_event.data.mouse_button.modifiers = sol_modifiers_from_ca(ev->mouse_button.mods);
    input_event.data.mouse_button.repeated = (ev->mouse_button.action == CA_REPEAT);

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_move(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    /* Cache for hit-testing in scroll handlers. */
    app->mouse_x = ev->mouse_pos.x;
    app->mouse_y = ev->mouse_pos.y;

    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_MOVE;
    input_event.data.mouse_move.x = ev->mouse_pos.x;
    input_event.data.mouse_move.y = ev->mouse_pos.y;
    input_event.data.mouse_move.delta_x = 0.0;
    input_event.data.mouse_move.delta_y = 0.0;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_SCROLL;
    input_event.data.mouse_scroll.x = (float)ev->mouse_scroll.dx;
    input_event.data.mouse_scroll.y = (float)ev->mouse_scroll.dy;

    sol_input_system_process_event(app->input, &input_event);

    /* Route the wheel to the pane under the cursor — even if it isn't
       the keyboard-focused one. This matches VS Code / browser behaviour
       and keeps mouse interaction natural. Falls back to the active
       leaf when the cursor isn't over a pane. */
    if (!app->buffers || !app->ui) return;

    int win_w = 0, win_h = 0;
    sol_ui_system_window_size(app->ui, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    const float title_h  = (float)sol_ui_system_title_bar_height(app->ui);
    const float status_h = (float)sol_ui_system_status_bar_height(app->ui);
    const float tree_w   = (float)sol_ui_system_tree_panel_width(app->ui);
    const float buf_x = tree_w;
    const float buf_y = title_h;
    const float buf_w = (float)win_w - tree_w;
    const float buf_h = (float)win_h - title_h - status_h;

    SolBufferId target_buffer = 0u;
    const double mx = app->mouse_x, my = app->mouse_y;
    if (mx >= buf_x && mx <= buf_x + buf_w &&
        my >= buf_y && my <= buf_y + buf_h)
    {
        SolBufferNodeId leaf = sol_buffer_leaf_at_point(
            app->buffers, buf_x, buf_y, buf_w, buf_h,
            /* split-bar size — keep in sync with workspace.c */ 1.0f,
            (float)mx, (float)my);
        if (leaf != 0u) {
            target_buffer = sol_buffer_leaf_buffer(app->buffers, leaf);
        }
    }
    if (target_buffer == 0u) {
        target_buffer = sol_buffer_active_buffer(app->buffers);
    }
    if (target_buffer == 0u) return;

    SolBuffer *buf = sol_buffer_get(app->buffers, target_buffer);
    if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) return;
    SolTextBufferState *ts = (SolTextBufferState *)sol_buffer_state(buf);
    if (!ts || ts->line_count == 0u) return;

    const int rendered = sol_text_visible_lines(win_h);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;
    const int total   = (int)ts->line_count;
    const int max_top = total > viewport ? total - viewport : 0;

    /* Natural scrolling: dy>0 scrolls content up (i.e. moves view down). */
    int delta = (int)(-ev->mouse_scroll.dy * 3.0);
    if (delta == 0) delta = ev->mouse_scroll.dy > 0.0 ? -1 : (ev->mouse_scroll.dy < 0.0 ? 1 : 0);

    int new_top = ts->scroll_top + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;

    if (new_top != ts->scroll_top) {
        ts->scroll_top = new_top;
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_ca_window_close(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_close(app->ui, ev->window);
}

static void sol_on_ca_window_resize(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_resize(app->ui, ev->resize.width, ev->resize.height);
}

int main(int argc, char **argv)
{
    SolAppContext app;
    memset(&app, 0, sizeof(app));

    SolSystemConfig system_config = sol_system_config_default();
    app.systems = sol_system_manager_create(&system_config);
    if (!app.systems) {
        fprintf(stderr, "Failed to create system manager\n");
        return 1;
    }

    app.events = sol_system_events(app.systems);
    app.buffers = sol_system_buffers(app.systems);
    app.jobs = sol_system_jobs(app.systems);
    app.input = sol_system_input(app.systems);

    /* No welcome / placeholder buffer: launching with neither a file
       nor a directory should leave the workspace empty (no tabs, no
       phantom "main" buffer). The first buffer is created lazily — by
       the CLI file path below, by File → Open, or by clicking a file
       in the tree panel. */

    /* CLI: `./sol <path>` opens the given file or directory.
     *   - file: load it into a fresh text buffer
     *   - dir : mount it as the file-tree root
     * The file-tree root is set after the UI system is created (below). */
    const char *cli_path = (argc >= 2 && argv[1] && argv[1][0] != '\0') ? argv[1] : NULL;
    bool cli_is_dir = false;
    if (cli_path) {
        struct stat st;
        if (stat(cli_path, &st) != 0) {
            fprintf(stderr, "sol: cannot stat '%s'\n", cli_path);
            sol_system_manager_destroy(app.systems);
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            cli_is_dir = true;
        } else if (!sol_open_path_in_active_leaf(app.buffers, cli_path)) {
            sol_system_manager_destroy(app.systems);
            return 1;
        }
    }

    app.startup_token = sol_event_bus_subscribe(app.events, &(SolEventSubscriptionDesc){
        .event_name = "core.startup",
        .priority = 100,
        .handler = sol_on_startup_event,
        .user_data = NULL,
    });

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .font_size_px         = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT,
    });
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        sol_system_manager_destroy(app.systems);
        return 1;
    }
    app.instance = instance;

    app.ui = sol_ui_system_create(instance, app.buffers);
    if (!app.ui) {
        fprintf(stderr, "Failed to create UI system\n");
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    /* File tree wiring: route clicks to a buffer-create + focus path,
     * and mount the root if a directory was given on the CLI. */
    sol_ui_system_set_file_open_callback(app.ui, sol_on_tree_file_open, app.buffers);
    if (cli_is_dir && cli_path) {
        if (!sol_ui_system_set_file_tree_root(app.ui, cli_path)) {
            fprintf(stderr, "sol: cannot open directory '%s'\n", cli_path);
        }
    }

    /* Title-bar File menu: Open File... / Open Folder... */
    sol_ui_system_install_menu(app.ui,
                               sol_on_menu_open_file,
                               sol_on_menu_open_folder,
                               &app);

    static const SolKeyCode flow_editor_save[] = { 'F', 'S' };
    static const SolKeyCode flow_workspace_split_vertical[] = { 'W', 'V' };
    static const SolKeyCode flow_workspace_split_horizontal[] = { 'W', 'H' };
    static const SolKeyCode flow_workspace_focus_next[] = { 'W', 'N' };
    static const SolKeyCode flow_buffer_next[] = { 'B', 'D' };
    static const SolKeyCode flow_buffer_prev[] = { 'B', 'A' };

    const bool save_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "editor.save",
        .label = "Save",
        .sequence = flow_editor_save,
        .sequence_length = 2u,
        .key = 'S',
        .callback = sol_ui_system_on_save_action,
        .user_data = app.ui,
    });

    const bool split_vertical_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.vertical",
        .label = "Split Vertical",
        .sequence = flow_workspace_split_vertical,
        .sequence_length = 2u,
        .key = 'V',
        .callback = sol_ui_system_on_split_vertical_action,
        .user_data = app.ui,
    });

    const bool split_horizontal_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.horizontal",
        .label = "Split Horizontal",
        .sequence = flow_workspace_split_horizontal,
        .sequence_length = 2u,
        .key = 'H',
        .callback = sol_ui_system_on_split_horizontal_action,
        .user_data = app.ui,
    });

    const bool focus_next_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.focus.next",
        .label = "Focus Next Pane",
        .sequence = flow_workspace_focus_next,
        .sequence_length = 2u,
        .key = 'N',
        .callback = sol_ui_system_on_focus_next_action,
        .user_data = app.ui,
    });

    const bool buffer_next_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "buffer.next",
        .label = "Next Buffer",
        .sequence = flow_buffer_next,
        .sequence_length = 2u,
        .key = 'D',
        .callback = sol_ui_system_on_buffer_next_action,
        .user_data = app.ui,
    });

    const bool buffer_prev_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "buffer.prev",
        .label = "Previous Buffer",
        .sequence = flow_buffer_prev,
        .sequence_length = 2u,
        .key = 'A',
        .callback = sol_ui_system_on_buffer_prev_action,
        .user_data = app.ui,
    });

    app.command_flows_ready = save_flow && split_vertical_flow && split_horizontal_flow
                             && focus_next_flow && buffer_next_flow && buffer_prev_flow;

    ca_event_set_handler(instance, CA_EVENT_KEY, sol_on_ca_key, &app);
    ca_event_set_handler(instance, CA_EVENT_CHAR, sol_on_ca_char, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON, sol_on_ca_mouse_button, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE, sol_on_ca_mouse_move, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL, sol_on_ca_mouse_scroll, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE, sol_on_ca_window_close, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, sol_on_ca_window_resize, &app);

    Ca_Window *window = sol_ui_system_primary_window(app.ui);
    if (!window) {
        fprintf(stderr, "Failed to access primary window\n");
        sol_ui_system_destroy(app.ui);
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    if (!sol_system_register_service(app.systems, "ca.instance", instance, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.instance service\n");
    }
    if (!sol_system_register_service(app.systems, "ca.window.primary", window, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.window.primary service\n");
    }

    SolWarmupContext warmup = { 0 };
    bool warmup_ok = sol_job_system_parallel_for(app.jobs, 100000u, 256u, sol_warmup_range, &warmup);

    const uint32_t loaded_plugins = (uint32_t)sol_system_load_plugins_from_directory(app.systems, NULL);

    const SolStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app.jobs),
        .loaded_plugins = loaded_plugins,
        .warmup_checksum = warmup_ok ? atomic_load_explicit(&warmup.checksum, memory_order_relaxed) : 0u,
        .input_binding_active = app.command_flows_ready,
    };

    sol_event_bus_post(app.events, &(SolEventDesc){
        .event_name = "core.startup",
        .payload = &startup,
        .payload_size = sizeof(startup),
        .sender = app.systems,
        .flags = SOL_EVENT_FLAG_NONE,
    });
    sol_system_pump_events(app.systems, 16u);

    for (;;) {
        sol_system_begin_frame(app.systems);
        if (!ca_instance_tick(instance)) {
            break;
        }
        sol_system_pump_events(app.systems, 128u);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }

    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    sol_ui_system_destroy(app.ui);

    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
