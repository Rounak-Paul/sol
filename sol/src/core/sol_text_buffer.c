// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_text_buffer.c — piece-table implementation. See header for design.

#include "sol_text_buffer.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================ */

typedef enum { PIECE_ORIG = 0, PIECE_ADD = 1 } PieceSrc;

typedef struct Piece {
    struct Piece *prev;
    struct Piece *next;
    PieceSrc      src;
    size_t        offset;   /* offset into source buffer */
    size_t        length;   /* bytes */
} Piece;

struct SolTextBuffer {
    /* Original buffer: owned, immutable after creation. */
    char  *orig_data;
    size_t orig_length;

    /* Add buffer: append-only. */
    char  *add_data;
    size_t add_length;
    size_t add_capacity;

    /* Piece list (sentinel-bounded). */
    Piece *head;
    Piece *tail;

    size_t   total_length;
    uint64_t generation;
    bool     dirty;
};

/* ============================================================
   Internal helpers
   ============================================================ */

static Piece *piece_new(PieceSrc src, size_t offset, size_t length)
{
    Piece *p = (Piece *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->src    = src;
    p->offset = offset;
    p->length = length;
    return p;
}

static void list_insert_after(SolTextBuffer *buf, Piece *anchor, Piece *p)
{
    p->prev = anchor;
    p->next = anchor->next;
    if (anchor->next) anchor->next->prev = p;
    else              buf->tail          = p;
    anchor->next = p;
}

static void list_remove(SolTextBuffer *buf, Piece *p)
{
    if (p->prev) p->prev->next = p->next; else buf->head = p->next;
    if (p->next) p->next->prev = p->prev; else buf->tail = p->prev;
    free(p);
}

/* Locate the piece containing document offset `pos` and the piece-relative
   offset `*piece_off` within it. Returns NULL on out-of-range. */
static Piece *find_piece(const SolTextBuffer *buf, size_t pos, size_t *piece_off)
{
    size_t cursor = 0;
    for (Piece *p = buf->head; p; p = p->next) {
        if (pos <= cursor + p->length) {
            *piece_off = pos - cursor;
            return p;
        }
        cursor += p->length;
    }
    return NULL;
}

static const char *piece_src_ptr(const SolTextBuffer *buf, const Piece *p)
{
    return (p->src == PIECE_ORIG ? buf->orig_data : buf->add_data) + p->offset;
}

static bool ensure_add_capacity(SolTextBuffer *buf, size_t need)
{
    if (buf->add_length + need <= buf->add_capacity) return true;
    size_t new_cap = buf->add_capacity ? buf->add_capacity : 4096;
    while (new_cap < buf->add_length + need) {
        size_t doubled = new_cap * 2;
        if (doubled <= new_cap) return false; /* overflow */
        new_cap = doubled;
    }
    char *grown = (char *)realloc(buf->add_data, new_cap);
    if (!grown) return false;
    buf->add_data     = grown;
    buf->add_capacity = new_cap;
    return true;
}

/* ============================================================
   Lifecycle
   ============================================================ */

SolTextBuffer *sol_text_buffer_create(void)
{
    return sol_text_buffer_create_from_owned(NULL, 0);
}

SolTextBuffer *sol_text_buffer_create_from_owned(char *bytes, size_t length)
{
    SolTextBuffer *buf = (SolTextBuffer *)calloc(1, sizeof(*buf));
    if (!buf) { free(bytes); return NULL; }
    buf->orig_data   = bytes;
    buf->orig_length = length;
    if (length > 0) {
        Piece *p = piece_new(PIECE_ORIG, 0, length);
        if (!p) { free(bytes); free(buf); return NULL; }
        buf->head = buf->tail = p;
        buf->total_length = length;
    }
    return buf;
}

void sol_text_buffer_destroy(SolTextBuffer *buf)
{
    if (!buf) return;
    Piece *p = buf->head;
    while (p) { Piece *n = p->next; free(p); p = n; }
    free(buf->orig_data);
    free(buf->add_data);
    free(buf);
}

/* ============================================================
   Queries
   ============================================================ */

size_t sol_text_buffer_length(const SolTextBuffer *buf)
{
    return buf ? buf->total_length : 0;
}

size_t sol_text_buffer_line_count(const SolTextBuffer *buf)
{
    if (!buf) return 0;
    size_t lines = 1;
    for (const Piece *p = buf->head; p; p = p->next) {
        const char *src = piece_src_ptr(buf, p);
        for (size_t i = 0; i < p->length; ++i)
            if (src[i] == '\n') lines++;
    }
    return lines;
}

size_t sol_text_buffer_read(const SolTextBuffer *buf,
                            size_t start,
                            char *out,
                            size_t max_bytes)
{
    if (!buf || !out || max_bytes == 0) return 0;
    if (start >= buf->total_length) return 0;

    size_t cursor = 0;
    size_t written = 0;
    for (const Piece *p = buf->head; p && written < max_bytes; p = p->next) {
        size_t piece_end = cursor + p->length;
        if (piece_end > start) {
            size_t local = (start > cursor) ? (start - cursor) : 0;
            size_t avail = p->length - local;
            size_t take  = avail;
            if (take > max_bytes - written) take = max_bytes - written;
            memcpy(out + written, piece_src_ptr(buf, p) + local, take);
            written += take;
            start   += take;
        }
        cursor = piece_end;
    }
    return written;
}

char *sol_text_buffer_to_cstring(const SolTextBuffer *buf)
{
    if (!buf) return NULL;
    char *out = (char *)malloc(buf->total_length + 1);
    if (!out) return NULL;
    size_t n = sol_text_buffer_read(buf, 0, out, buf->total_length);
    out[n] = '\0';
    return out;
}

/* ============================================================
   Mutations
   ============================================================ */

bool sol_text_buffer_insert(SolTextBuffer *buf,
                            size_t offset,
                            const char *bytes,
                            size_t length)
{
    if (!buf || !bytes || length == 0) return false;
    if (offset > buf->total_length) return false;

    if (!ensure_add_capacity(buf, length)) return false;
    size_t add_off = buf->add_length;
    memcpy(buf->add_data + add_off, bytes, length);
    buf->add_length += length;

    /* Locate insertion point. */
    if (!buf->head) {
        /* Empty document. */
        Piece *p = piece_new(PIECE_ADD, add_off, length);
        if (!p) return false;
        buf->head = buf->tail = p;
        buf->total_length += length;
        buf->generation++;
        buf->dirty = true;
        return true;
    }

    size_t piece_off = 0;
    Piece *p = find_piece(buf, offset, &piece_off);
    if (!p) return false;

    /* Fast path: append to an existing tail-of-add piece — keeps piece
       count bounded during sustained typing. */
    if (piece_off == p->length &&
        p->src == PIECE_ADD &&
        p->offset + p->length == add_off) {
        p->length         += length;
        buf->total_length += length;
        buf->generation++;
        buf->dirty = true;
        return true;
    }

    Piece *new_p = piece_new(PIECE_ADD, add_off, length);
    if (!new_p) return false;

    if (piece_off == 0) {
        /* Insert before p. */
        new_p->next = p;
        new_p->prev = p->prev;
        if (p->prev) p->prev->next = new_p; else buf->head = new_p;
        p->prev = new_p;
    } else if (piece_off == p->length) {
        /* Insert after p. */
        list_insert_after(buf, p, new_p);
    } else {
        /* Split p around piece_off. */
        Piece *tail = piece_new(p->src, p->offset + piece_off, p->length - piece_off);
        if (!tail) { free(new_p); return false; }
        p->length = piece_off;
        list_insert_after(buf, p, new_p);
        list_insert_after(buf, new_p, tail);
    }

    buf->total_length += length;
    buf->generation++;
    buf->dirty = true;
    return true;
}

bool sol_text_buffer_erase(SolTextBuffer *buf, size_t offset, size_t length)
{
    if (!buf || length == 0) return false;
    if (offset >= buf->total_length) return false;
    if (offset + length > buf->total_length)
        length = buf->total_length - offset;

    /* Walk pieces, trimming/removing until `length` bytes are gone. */
    size_t remaining = length;
    size_t piece_off = 0;
    Piece *p = find_piece(buf, offset, &piece_off);
    if (!p) return false;

    while (p && remaining > 0) {
        Piece *next = p->next;
        size_t avail = p->length - piece_off;

        if (piece_off == 0 && remaining >= avail) {
            /* Drop whole piece. */
            remaining -= avail;
            list_remove(buf, p);
        } else if (piece_off == 0) {
            /* Trim from front. */
            p->offset += remaining;
            p->length -= remaining;
            remaining  = 0;
        } else if (remaining >= avail) {
            /* Trim from tail of this piece. */
            p->length  = piece_off;
            remaining -= avail;
        } else {
            /* Middle: split into [0..piece_off) and [piece_off+remaining..len). */
            Piece *tail = piece_new(p->src,
                                    p->offset + piece_off + remaining,
                                    p->length - piece_off - remaining);
            if (!tail) return false;
            p->length = piece_off;
            list_insert_after(buf, p, tail);
            remaining = 0;
        }

        p = next;
        piece_off = 0;
    }

    buf->total_length -= (length - remaining);
    buf->generation++;
    buf->dirty = true;
    return true;
}

bool sol_text_buffer_insert_char(SolTextBuffer *buf, size_t offset, uint32_t cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80)        { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800)  { tmp[0] = (char)(0xC0 | (cp >> 6));
                            tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000){ tmp[0] = (char)(0xE0 | (cp >> 12));
                            tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else if (cp < 0x110000){tmp[0] = (char)(0xF0 | (cp >> 18));
                            tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    else return false;
    return sol_text_buffer_insert(buf, offset, tmp, n);
}

/* ============================================================
   Dirty tracking
   ============================================================ */

bool     sol_text_buffer_dirty     (const SolTextBuffer *buf) { return buf && buf->dirty; }
void     sol_text_buffer_mark_clean(SolTextBuffer       *buf) { if (buf) buf->dirty = false; }
uint64_t sol_text_buffer_generation(const SolTextBuffer *buf) { return buf ? buf->generation : 0; }
