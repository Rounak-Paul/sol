#include "sol_buffer.h"
#include "sol_event.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   INTERNAL TYPES
   ============================================================ */

/* One line of text — heap-allocated, always null-terminated.
   `len` is the byte count NOT counting the null terminator.
   `cap` is the allocated capacity INCLUDING the null terminator. */
typedef struct {
    char    *data;
    uint32_t len;
    uint32_t cap;
} BufLine;

struct Sol_Buffer {
    uint32_t  id;
    char     *path;     /* heap-allocated, NULL = unnamed */
    bool      dirty;
    Sol_Mode  mode;

    BufLine  *lines;
    uint32_t  line_count;
    uint32_t  line_cap;

    /* Cursor — always kept within valid bounds */
    uint32_t  cursor_line;
    uint32_t  cursor_col;   /* byte offset within the line */

    /* Selection */
    bool      has_selection;
    uint32_t  sel_anchor_line;
    uint32_t  sel_anchor_col;
};

/* ============================================================
   GLOBAL REGISTRY
   ============================================================ */

#define SOL_MAX_BUFFERS 256u

typedef struct {
    Sol_Buffer *slots[SOL_MAX_BUFFERS];
    uint32_t    count;
    uint32_t    next_id; /* starts at 1 */
} BufferRegistry;

static BufferRegistry g_reg;

void sol_buffers_init(void)
{
    memset(&g_reg, 0, sizeof(g_reg));
    g_reg.next_id = 1u;
}

void sol_buffers_shutdown(void)
{
    for (uint32_t i = 0; i < SOL_MAX_BUFFERS; i++) {
        if (g_reg.slots[i])
            sol_buffer_destroy(g_reg.slots[i]);
    }
    memset(&g_reg, 0, sizeof(g_reg));
}

Sol_Buffer *sol_buffer_get_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < SOL_MAX_BUFFERS; i++) {
        if (g_reg.slots[i] && g_reg.slots[i]->id == id)
            return g_reg.slots[i];
    }
    return NULL;
}

static void registry_add(Sol_Buffer *buf)
{
    for (uint32_t i = 0; i < SOL_MAX_BUFFERS; i++) {
        if (!g_reg.slots[i]) {
            g_reg.slots[i] = buf;
            g_reg.count++;
            return;
        }
    }
    assert(false && "sol: buffer registry full");
}

static void registry_remove(Sol_Buffer *buf)
{
    for (uint32_t i = 0; i < SOL_MAX_BUFFERS; i++) {
        if (g_reg.slots[i] == buf) {
            g_reg.slots[i] = NULL;
            g_reg.count--;
            return;
        }
    }
}

/* ============================================================
   BUFLINE HELPERS
   ============================================================ */

static void line_ensure_cap(BufLine *l, uint32_t needed_data_cap)
{
    if (needed_data_cap <= l->cap) return;
    uint32_t new_cap = l->cap ? l->cap * 2u : 16u;
    while (new_cap < needed_data_cap) new_cap *= 2u;
    l->data = realloc(l->data, new_cap);
    assert(l->data && "sol: buffer line allocation failed");
    l->cap = new_cap;
}

/* Creates a new BufLine owning a copy of `text` (NOT null-terminated input). */
static BufLine line_from_bytes(const char *text, uint32_t len)
{
    BufLine l = {0};
    line_ensure_cap(&l, len + 1u);
    if (len > 0) memcpy(l.data, text, len);
    l.data[len] = '\0';
    l.len = len;
    return l;
}

static void line_free(BufLine *l)
{
    free(l->data);
    l->data = NULL;
    l->len = l->cap = 0;
}

/* Insert `len` bytes at byte position `col` within a BufLine. */
static void line_insert_bytes(BufLine *l, uint32_t col, const char *bytes, uint32_t len)
{
    if (col > l->len) col = l->len;
    line_ensure_cap(l, l->len + len + 1u);
    /* Shift suffix right. */
    memmove(l->data + col + len, l->data + col, l->len - col + 1u /* incl. null */);
    memcpy(l->data + col, bytes, len);
    l->len += len;
}

/* Delete bytes in [col1, col2) from a BufLine. */
static void line_delete_bytes(BufLine *l, uint32_t col1, uint32_t col2)
{
    if (col1 >= l->len) return;
    if (col2 > l->len) col2 = l->len;
    if (col1 >= col2) return;
    uint32_t del = col2 - col1;
    memmove(l->data + col1, l->data + col2, l->len - col2 + 1u /* incl. null */);
    l->len -= del;
}

/* Append `len` bytes from `src` to line `l`. */
static void line_append(BufLine *l, const char *src, uint32_t len)
{
    line_ensure_cap(l, l->len + len + 1u);
    memcpy(l->data + l->len, src, len);
    l->len += len;
    l->data[l->len] = '\0';
}

/* ============================================================
   LINE ARRAY HELPERS
   ============================================================ */

static void lines_ensure_cap(Sol_Buffer *buf, uint32_t needed)
{
    if (needed <= buf->line_cap) return;
    uint32_t new_cap = buf->line_cap ? buf->line_cap * 2u : 16u;
    while (new_cap < needed) new_cap *= 2u;
    buf->lines = realloc(buf->lines, new_cap * sizeof(BufLine));
    assert(buf->lines && "sol: buffer line array allocation failed");
    buf->line_cap = new_cap;
}

/* Insert `count` blank-initialised BufLine slots before `at`. */
static void lines_insert_empty(Sol_Buffer *buf, uint32_t at, uint32_t count)
{
    lines_ensure_cap(buf, buf->line_count + count);
    memmove(&buf->lines[at + count], &buf->lines[at],
            (buf->line_count - at) * sizeof(BufLine));
    memset(&buf->lines[at], 0, count * sizeof(BufLine));
    buf->line_count += count;
}

/* Remove lines [from, from+count). */
static void lines_remove(Sol_Buffer *buf, uint32_t from, uint32_t count)
{
    for (uint32_t i = from; i < from + count; i++) line_free(&buf->lines[i]);
    memmove(&buf->lines[from], &buf->lines[from + count],
            (buf->line_count - from - count) * sizeof(BufLine));
    buf->line_count -= count;
}

/* ============================================================
   CURSOR CLAMPING
   ============================================================ */

static void clamp_cursor(Sol_Buffer *buf)
{
    if (buf->cursor_line >= buf->line_count)
        buf->cursor_line = buf->line_count > 0 ? buf->line_count - 1u : 0u;
    uint32_t llen = buf->lines[buf->cursor_line].len;
    if (buf->cursor_col > llen)
        buf->cursor_col = llen;
}

/* ============================================================
   LIFECYCLE
   ============================================================ */

static Sol_Buffer *buf_alloc(void)
{
    Sol_Buffer *buf = calloc(1, sizeof(Sol_Buffer));
    assert(buf && "sol: buffer allocation failed");
    buf->id   = g_reg.next_id++;
    buf->mode = SOL_MODE_NORMAL;
    return buf;
}

Sol_Buffer *sol_buffer_create(void)
{
    Sol_Buffer *buf = buf_alloc();
    /* Start with one empty line. */
    lines_ensure_cap(buf, 1u);
    buf->lines[0] = line_from_bytes("", 0u);
    buf->line_count = 1u;

    registry_add(buf);

    sol_event_emit(SOL_EVENT_BUFFER_OPEN, &(Sol_BufferEvent){
        .buffer_id = buf->id,
        .path      = NULL,
    });

    return buf;
}

Sol_Buffer *sol_buffer_open(const char *path)
{
    assert(path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sol: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    /* Read entire file into a temporary buffer. */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size < 0) {
        fclose(f);
        return NULL;
    }

    char *raw = malloc((size_t)file_size + 1u);
    assert(raw);
    size_t read_bytes = fread(raw, 1, (size_t)file_size, f);
    raw[read_bytes] = '\0';
    fclose(f);

    Sol_Buffer *buf = buf_alloc();
    buf->path = strdup(path);

    /* Parse into lines, normalising CRLF → LF, stripping bare CR. */
    const char *p   = raw;
    const char *end = raw + read_bytes;

    while (p <= end) {
        const char *line_start = p;
        while (p < end && *p != '\n' && *p != '\r') p++;

        uint32_t line_len = (uint32_t)(p - line_start);

        lines_ensure_cap(buf, buf->line_count + 1u);
        buf->lines[buf->line_count] = line_from_bytes(line_start, line_len);
        buf->line_count++;

        if (p < end) {
            if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') p += 2; /* CRLF */
            else p++;  /* LF or bare CR */
        } else {
            break;
        }
    }

    /* Ensure at least one line. */
    if (buf->line_count == 0) {
        lines_ensure_cap(buf, 1u);
        buf->lines[0] = line_from_bytes("", 0u);
        buf->line_count = 1u;
    }

    free(raw);

    registry_add(buf);

    sol_event_emit(SOL_EVENT_BUFFER_OPEN, &(Sol_BufferEvent){
        .buffer_id = buf->id,
        .path      = path,
    });

    return buf;
}

void sol_buffer_destroy(Sol_Buffer *buf)
{
    if (!buf) return;

    sol_event_emit(SOL_EVENT_BUFFER_CLOSE, &(Sol_BufferEvent){
        .buffer_id = buf->id,
        .path      = buf->path,
    });

    registry_remove(buf);

    for (uint32_t i = 0; i < buf->line_count; i++) line_free(&buf->lines[i]);
    free(buf->lines);
    free(buf->path);
    free(buf);
}

/* ============================================================
   PROPERTIES
   ============================================================ */

uint32_t sol_buffer_id(const Sol_Buffer *buf)   { return buf->id; }
const char *sol_buffer_path(const Sol_Buffer *buf) { return buf->path; }
bool sol_buffer_is_dirty(const Sol_Buffer *buf) { return buf->dirty; }
Sol_Mode sol_buffer_mode(const Sol_Buffer *buf) { return buf->mode; }

void sol_buffer_set_mode(Sol_Buffer *buf, Sol_Mode mode)
{
    buf->mode = mode;
    /* In NORMAL mode, ensure cursor is not past end of line. */
    if (mode == SOL_MODE_NORMAL) {
        uint32_t llen = buf->lines[buf->cursor_line].len;
        if (llen > 0 && buf->cursor_col >= llen)
            buf->cursor_col = llen - 1u;
    }
}

/* ============================================================
   CONTENT ACCESS
   ============================================================ */

uint32_t sol_buffer_line_count(const Sol_Buffer *buf) { return buf->line_count; }

const char *sol_buffer_line(const Sol_Buffer *buf, uint32_t line, uint32_t *out_len)
{
    if (line >= buf->line_count) return NULL;
    if (out_len) *out_len = buf->lines[line].len;
    return buf->lines[line].data;
}

uint32_t sol_buffer_total_bytes(const Sol_Buffer *buf)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < buf->line_count; i++)
        total += buf->lines[i].len;
    return total;
}

/* ============================================================
   EDITING
   ============================================================ */

void sol_buffer_insert(Sol_Buffer *buf,
                       uint32_t line, uint32_t col,
                       const char *text, uint32_t len)
{
    if (!text || len == 0) return;
    if (line >= buf->line_count) line = buf->line_count - 1u;

    /* Scan the text for newlines, collecting segments. */
    const char *p   = text;
    const char *end = text + len;

    /* First segment: insert into `line` at `col`. */
    const char *seg_start = p;
    while (p < end && *p != '\n' && *p != '\r') p++;
    uint32_t seg_len = (uint32_t)(p - seg_start);

    if (p == end) {
        /* No newlines — simple inline insert. */
        if (col > buf->lines[line].len) col = buf->lines[line].len;
        line_insert_bytes(&buf->lines[line], col, seg_start, seg_len);
        buf->dirty = true;
        sol_event_emit(SOL_EVENT_BUFFER_CHANGE, &(Sol_ChangeEvent){
            .buffer_id    = buf->id,
            .offset       = col,
            .removed_len  = 0,
            .inserted     = text,
            .inserted_len = len,
        });
        return;
    }

    /* There is at least one newline: split `line` at `col`, handle segments. */
    if (col > buf->lines[line].len) col = buf->lines[line].len;

    /* Save the suffix of the current line that will become the last new line's tail. */
    uint32_t tail_len = buf->lines[line].len - col;
    char *tail = NULL;
    if (tail_len > 0) {
        tail = malloc(tail_len);
        assert(tail);
        memcpy(tail, buf->lines[line].data + col, tail_len);
    }

    /* Truncate the current line at `col`, append the first segment. */
    buf->lines[line].len = col;
    buf->lines[line].data[col] = '\0';
    line_insert_bytes(&buf->lines[line], col, seg_start, seg_len);

    /* Skip past the newline(s). */
    if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') p += 2;
    else p++;

    uint32_t insert_after = line; /* line index to insert new lines after */

    /* Process remaining segments (each newline creates a new line). */
    while (p <= end) {
        insert_after++;

        seg_start = p;
        while (p < end && *p != '\n' && *p != '\r') p++;
        seg_len = (uint32_t)(p - seg_start);

        bool is_last_seg = (p >= end);

        lines_insert_empty(buf, insert_after, 1u);
        if (is_last_seg) {
            /* Last segment: append the saved tail. */
            buf->lines[insert_after] = line_from_bytes(seg_start, seg_len);
            if (tail_len > 0) line_append(&buf->lines[insert_after], tail, tail_len);
        } else {
            buf->lines[insert_after] = line_from_bytes(seg_start, seg_len);
        }

        if (is_last_seg) break;

        if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') p += 2;
        else p++;
    }

    free(tail);
    buf->dirty = true;

    sol_event_emit(SOL_EVENT_BUFFER_CHANGE, &(Sol_ChangeEvent){
        .buffer_id    = buf->id,
        .offset       = col,
        .removed_len  = 0,
        .inserted     = text,
        .inserted_len = len,
    });
}

void sol_buffer_delete(Sol_Buffer *buf,
                       uint32_t line1, uint32_t col1,
                       uint32_t line2, uint32_t col2)
{
    if (line1 >= buf->line_count) return;
    if (line2 >= buf->line_count) {
        line2 = buf->line_count - 1u;
        col2  = buf->lines[line2].len;
    }

    /* Clamp columns. */
    if (col1 > buf->lines[line1].len) col1 = buf->lines[line1].len;
    if (col2 > buf->lines[line2].len) col2 = buf->lines[line2].len;

    if (line1 == line2) {
        if (col1 >= col2) return;
        line_delete_bytes(&buf->lines[line1], col1, col2);
    } else {
        /* Save lines[line2] suffix from col2 onward. */
        uint32_t tail_len = buf->lines[line2].len - col2;
        char *tail = NULL;
        if (tail_len > 0) {
            tail = malloc(tail_len);
            assert(tail);
            memcpy(tail, buf->lines[line2].data + col2, tail_len);
        }

        /* Truncate line1 at col1, append tail. */
        buf->lines[line1].len = col1;
        buf->lines[line1].data[col1] = '\0';
        if (tail_len > 0) line_append(&buf->lines[line1], tail, tail_len);
        free(tail);

        /* Remove lines line1+1 through line2 (inclusive). */
        lines_remove(buf, line1 + 1u, line2 - line1);
    }

    buf->dirty = true;

    sol_event_emit(SOL_EVENT_BUFFER_CHANGE, &(Sol_ChangeEvent){
        .buffer_id    = buf->id,
        .offset       = col1,
        .removed_len  = col1, /* approximate; full offset calc omitted */
        .inserted     = NULL,
        .inserted_len = 0,
    });
}

/* ============================================================
   CURSOR
   ============================================================ */

void sol_buffer_cursor_get(const Sol_Buffer *buf,
                           uint32_t *out_line, uint32_t *out_col)
{
    if (out_line) *out_line = buf->cursor_line;
    if (out_col)  *out_col  = buf->cursor_col;
}

void sol_buffer_cursor_set(Sol_Buffer *buf, uint32_t line, uint32_t col)
{
    if (line >= buf->line_count)
        line = buf->line_count > 0 ? buf->line_count - 1u : 0u;
    uint32_t llen = buf->lines[line].len;
    if (col > llen) col = llen;

    buf->cursor_line = line;
    buf->cursor_col  = col;

    sol_event_emit(SOL_EVENT_CURSOR_MOVE, &(Sol_CursorEvent){
        .buffer_id = buf->id,
        .line      = line,
        .column    = col,
        .offset    = 0, /* full offset calculation left for future */
    });
}

void sol_buffer_cursor_move(Sol_Buffer *buf, int dl, int dc)
{
    int32_t new_line = (int32_t)buf->cursor_line + dl;
    if (new_line < 0) new_line = 0;
    if ((uint32_t)new_line >= buf->line_count) new_line = (int32_t)(buf->line_count - 1u);

    int32_t new_col = (int32_t)buf->cursor_col + dc;
    uint32_t llen = buf->lines[(uint32_t)new_line].len;
    if (new_col < 0) new_col = 0;
    if ((uint32_t)new_col > llen) new_col = (int32_t)llen;

    sol_buffer_cursor_set(buf, (uint32_t)new_line, (uint32_t)new_col);
}

/* ============================================================
   SELECTION
   ============================================================ */

bool sol_buffer_has_selection(const Sol_Buffer *buf) { return buf->has_selection; }

void sol_buffer_selection_get(const Sol_Buffer *buf,
                               uint32_t *anchor_line, uint32_t *anchor_col,
                               uint32_t *active_line, uint32_t *active_col)
{
    if (anchor_line) *anchor_line = buf->sel_anchor_line;
    if (anchor_col)  *anchor_col  = buf->sel_anchor_col;
    if (active_line) *active_line = buf->cursor_line;
    if (active_col)  *active_col  = buf->cursor_col;
}

void sol_buffer_selection_clear(Sol_Buffer *buf)
{
    buf->has_selection = false;
}

/* ============================================================
   I / O
   ============================================================ */

static bool write_buffer_to_path(Sol_Buffer *buf, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "sol: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }
    for (uint32_t i = 0; i < buf->line_count; i++) {
        fwrite(buf->lines[i].data, 1, buf->lines[i].len, f);
        if (i + 1 < buf->line_count) fputc('\n', f);
    }
    fclose(f);
    return true;
}

bool sol_buffer_save(Sol_Buffer *buf)
{
    if (!buf->path) return false;
    if (!write_buffer_to_path(buf, buf->path)) return false;
    buf->dirty = false;
    sol_event_emit(SOL_EVENT_BUFFER_SAVE, &(Sol_BufferEvent){
        .buffer_id = buf->id,
        .path      = buf->path,
    });
    return true;
}

bool sol_buffer_save_as(Sol_Buffer *buf, const char *path)
{
    assert(path);
    if (!write_buffer_to_path(buf, path)) return false;
    free(buf->path);
    buf->path  = strdup(path);
    buf->dirty = false;
    sol_event_emit(SOL_EVENT_BUFFER_SAVE, &(Sol_BufferEvent){
        .buffer_id = buf->id,
        .path      = buf->path,
    });
    return true;
}
