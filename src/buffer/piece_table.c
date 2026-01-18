/*
 * Sol IDE - Piece Table Implementation
 * 
 * A piece table is an efficient data structure for text editing.
 * It maintains two buffers:
 *   - Original: immutable content from the file
 *   - Add: append-only buffer for new text
 * 
 * Text is represented as a linked list of "pieces" that point to
 * either buffer. Inserts and deletes only modify the piece list,
 * making them O(1) to O(n) where n is the number of pieces, not
 * the size of the document.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Helper: count newlines in a text range */
static int32_t count_newlines(const char* text, size_t len) {
    int32_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') count++;
    }
    return count;
}

/* Get the text buffer for a piece */
static const char* piece_buffer(sol_piece_table* pt, sol_piece* piece) {
    if (piece->source == SOL_PIECE_ORIGINAL) {
        return pt->original + piece->start;
    } else {
        return pt->add + piece->start;
    }
}

/* Create a new piece */
static sol_piece* piece_create(sol_piece_table* pt, sol_piece_source source, 
                                size_t start, size_t length) {
    sol_piece* piece = (sol_piece*)sol_arena_alloc_zero(pt->piece_arena, sizeof(sol_piece));
    if (!piece) return NULL;
    
    piece->source = source;
    piece->start = start;
    piece->length = length;
    
    /* Cache line count */
    const char* buf = source == SOL_PIECE_ORIGINAL ? pt->original : pt->add;
    piece->line_count = count_newlines(buf + start, length);
    
    return piece;
}

/* Insert piece after another */
static void piece_insert_after(sol_piece_table* pt, sol_piece* after, sol_piece* piece) {
    piece->prev = after;
    piece->next = after->next;
    
    if (after->next) {
        after->next->prev = piece;
    } else {
        pt->last = piece;
    }
    after->next = piece;
}

/* Insert piece before another */
static void piece_insert_before(sol_piece_table* pt, sol_piece* before, sol_piece* piece) {
    piece->next = before;
    piece->prev = before->prev;
    
    if (before->prev) {
        before->prev->next = piece;
    } else {
        pt->first = piece;
    }
    before->prev = piece;
}

/* Remove piece from list (doesn't free - arena manages memory) */
static void piece_remove(sol_piece_table* pt, sol_piece* piece) {
    if (piece->prev) {
        piece->prev->next = piece->next;
    } else {
        pt->first = piece->next;
    }
    
    if (piece->next) {
        piece->next->prev = piece->prev;
    } else {
        pt->last = piece->prev;
    }
    
    pt->total_length -= piece->length;
    pt->total_lines -= piece->line_count;
}

/* Find piece containing offset, and relative offset within piece */
static sol_piece* find_piece_at_offset(sol_piece_table* pt, size_t offset, 
                                        size_t* piece_offset) {
    size_t pos = 0;
    sol_piece* piece = pt->first;
    
    while (piece) {
        if (pos + piece->length > offset || !piece->next) {
            if (piece_offset) {
                *piece_offset = offset - pos;
            }
            return piece;
        }
        pos += piece->length;
        piece = piece->next;
    }
    
    return NULL;
}

/* Ensure add buffer has space */
static bool ensure_add_capacity(sol_piece_table* pt, size_t additional) {
    size_t needed = pt->add_len + additional;
    
    if (needed <= pt->add_cap) {
        return true;
    }
    
    size_t new_cap = pt->add_cap * 2;
    if (new_cap < needed) new_cap = needed;
    if (new_cap < 4096) new_cap = 4096;
    
    char* new_add = (char*)realloc(pt->add, new_cap);
    if (!new_add) return false;
    
    pt->add = new_add;
    pt->add_cap = new_cap;
    
    return true;
}

sol_piece_table* sol_piece_table_create(const char* initial, size_t len) {
    sol_piece_table* pt = (sol_piece_table*)calloc(1, sizeof(sol_piece_table));
    if (!pt) return NULL;
    
    pt->piece_arena = sol_arena_create();
    if (!pt->piece_arena) {
        free(pt);
        return NULL;
    }
    
    /* Copy original content */
    if (initial && len > 0) {
        pt->original = (char*)malloc(len);
        if (!pt->original) {
            sol_arena_destroy(pt->piece_arena);
            free(pt);
            return NULL;
        }
        memcpy(pt->original, initial, len);
        pt->original_len = len;
        
        /* Create initial piece */
        sol_piece* piece = piece_create(pt, SOL_PIECE_ORIGINAL, 0, len);
        if (!piece) {
            free(pt->original);
            sol_arena_destroy(pt->piece_arena);
            free(pt);
            return NULL;
        }
        
        pt->first = piece;
        pt->last = piece;
        pt->total_length = len;
        pt->total_lines = piece->line_count + 1;  /* +1 for line after last newline */
    } else {
        pt->total_lines = 1;  /* Empty buffer has 1 line */
    }
    
    /* Initialize add buffer */
    pt->add_cap = 4096;
    pt->add = (char*)malloc(pt->add_cap);
    if (!pt->add) {
        free(pt->original);
        sol_arena_destroy(pt->piece_arena);
        free(pt);
        return NULL;
    }
    pt->add_len = 0;
    
    return pt;
}

void sol_piece_table_destroy(sol_piece_table* pt) {
    if (!pt) return;
    
    free(pt->original);
    free(pt->add);
    sol_arena_destroy(pt->piece_arena);
    free(pt);
}

sol_result sol_piece_table_insert(sol_piece_table* pt, size_t offset, 
                                   const char* text, size_t len) {
    if (!pt || !text || len == 0) return SOL_ERROR_INVALID_ARG;
    
    /* Clamp offset */
    if (offset > pt->total_length) {
        offset = pt->total_length;
    }
    
    /* Append to add buffer */
    if (!ensure_add_capacity(pt, len)) {
        return SOL_ERROR_MEMORY;
    }
    
    size_t add_start = pt->add_len;
    memcpy(pt->add + pt->add_len, text, len);
    pt->add_len += len;
    
    /* Create new piece for inserted text */
    sol_piece* new_piece = piece_create(pt, SOL_PIECE_ADD, add_start, len);
    if (!new_piece) return SOL_ERROR_MEMORY;
    
    if (!pt->first) {
        /* Empty document */
        pt->first = new_piece;
        pt->last = new_piece;
    } else if (offset == 0) {
        /* Insert at beginning */
        piece_insert_before(pt, pt->first, new_piece);
    } else if (offset == pt->total_length) {
        /* Insert at end */
        piece_insert_after(pt, pt->last, new_piece);
    } else {
        /* Insert in middle */
        size_t piece_offset;
        sol_piece* piece = find_piece_at_offset(pt, offset, &piece_offset);
        
        if (!piece) return SOL_ERROR_UNKNOWN;
        
        if (piece_offset == 0) {
            /* Insert before this piece */
            piece_insert_before(pt, piece, new_piece);
        } else if (piece_offset == piece->length) {
            /* Insert after this piece */
            piece_insert_after(pt, piece, new_piece);
        } else {
            /* Split the piece */
            sol_piece* right = piece_create(pt, piece->source, 
                                            piece->start + piece_offset,
                                            piece->length - piece_offset);
            if (!right) return SOL_ERROR_MEMORY;
            
            /* Update left piece */
            pt->total_lines -= piece->line_count;
            piece->length = piece_offset;
            piece->line_count = count_newlines(piece_buffer(pt, piece), piece->length);
            pt->total_lines += piece->line_count;
            
            /* Insert new piece and right split */
            piece_insert_after(pt, piece, new_piece);
            piece_insert_after(pt, new_piece, right);
            pt->total_lines += right->line_count;
        }
    }
    
    pt->total_length += len;
    pt->total_lines += new_piece->line_count;
    
    return SOL_OK;
}

sol_result sol_piece_table_delete(sol_piece_table* pt, size_t offset, size_t len) {
    if (!pt) return SOL_ERROR_INVALID_ARG;
    if (len == 0) return SOL_OK;
    if (offset >= pt->total_length) return SOL_ERROR_INVALID_ARG;
    
    /* Clamp length */
    if (offset + len > pt->total_length) {
        len = pt->total_length - offset;
    }
    
    size_t remaining = len;
    size_t piece_offset;
    sol_piece* piece = find_piece_at_offset(pt, offset, &piece_offset);
    
    while (piece && remaining > 0) {
        size_t piece_remaining = piece->length - piece_offset;
        sol_piece* next = piece->next;
        
        if (piece_offset == 0 && remaining >= piece->length) {
            /* Delete entire piece */
            remaining -= piece->length;
            piece_remove(pt, piece);
        } else if (piece_offset == 0) {
            /* Delete from start of piece */
            pt->total_lines -= piece->line_count;
            const char* buf = piece_buffer(pt, piece);
            pt->total_lines -= count_newlines(buf, remaining);
            
            piece->start += remaining;
            piece->length -= remaining;
            piece->line_count = count_newlines(piece_buffer(pt, piece), piece->length);
            pt->total_length -= remaining;
            pt->total_lines += piece->line_count;
            remaining = 0;
        } else if (piece_offset + remaining >= piece->length) {
            /* Delete from middle to end */
            size_t to_delete = piece->length - piece_offset;
            pt->total_lines -= piece->line_count;
            
            piece->length = piece_offset;
            piece->line_count = count_newlines(piece_buffer(pt, piece), piece->length);
            pt->total_length -= to_delete;
            pt->total_lines += piece->line_count;
            remaining -= to_delete;
        } else {
            /* Delete from middle (need to split) */
            sol_piece* right = piece_create(pt, piece->source,
                                            piece->start + piece_offset + remaining,
                                            piece->length - piece_offset - remaining);
            if (!right) return SOL_ERROR_MEMORY;
            
            pt->total_lines -= piece->line_count;
            piece->length = piece_offset;
            piece->line_count = count_newlines(piece_buffer(pt, piece), piece->length);
            pt->total_length -= remaining;
            pt->total_lines += piece->line_count;
            
            piece_insert_after(pt, piece, right);
            remaining = 0;
        }
        
        piece = next;
        piece_offset = 0;
    }
    
    return SOL_OK;
}

size_t sol_piece_table_length(sol_piece_table* pt) {
    return pt ? pt->total_length : 0;
}

int32_t sol_piece_table_line_count(sol_piece_table* pt) {
    return pt ? pt->total_lines : 0;
}

char sol_piece_table_char_at(sol_piece_table* pt, size_t offset) {
    if (!pt || offset >= pt->total_length) return '\0';
    
    size_t piece_offset;
    sol_piece* piece = find_piece_at_offset(pt, offset, &piece_offset);
    
    if (!piece) return '\0';
    
    return piece_buffer(pt, piece)[piece_offset];
}

size_t sol_piece_table_get_text(sol_piece_table* pt, size_t offset, 
                                 size_t len, char* out) {
    if (!pt || !out || offset >= pt->total_length) return 0;
    
    if (offset + len > pt->total_length) {
        len = pt->total_length - offset;
    }
    
    size_t piece_offset;
    sol_piece* piece = find_piece_at_offset(pt, offset, &piece_offset);
    
    size_t written = 0;
    while (piece && written < len) {
        const char* buf = piece_buffer(pt, piece);
        size_t available = piece->length - piece_offset;
        size_t to_copy = len - written;
        if (to_copy > available) to_copy = available;
        
        memcpy(out + written, buf + piece_offset, to_copy);
        written += to_copy;
        
        piece = piece->next;
        piece_offset = 0;
    }
    
    return written;
}

size_t sol_piece_table_line_start(sol_piece_table* pt, int32_t line) {
    if (!pt || line < 0 || line >= pt->total_lines) return 0;
    
    if (line == 0) return 0;
    
    size_t offset = 0;
    int32_t current_line = 0;
    sol_piece* piece = pt->first;
    
    while (piece && current_line < line) {
        const char* buf = piece_buffer(pt, piece);
        
        for (size_t i = 0; i < piece->length && current_line < line; i++) {
            if (buf[i] == '\n') {
                current_line++;
                if (current_line == line) {
                    return offset + i + 1;
                }
            }
        }
        
        offset += piece->length;
        piece = piece->next;
    }
    
    return offset;
}

size_t sol_piece_table_get_line(sol_piece_table* pt, int32_t line, 
                                 char* out, size_t out_size) {
    if (!pt || !out || out_size == 0) return 0;
    
    size_t start = sol_piece_table_line_start(pt, line);
    size_t len = sol_piece_table_line_length(pt, line);
    
    if (len >= out_size) len = out_size - 1;
    
    size_t read = sol_piece_table_get_text(pt, start, len, out);
    out[read] = '\0';
    
    return read;
}

size_t sol_piece_table_line_length(sol_piece_table* pt, int32_t line) {
    if (!pt || line < 0 || line >= pt->total_lines) return 0;
    
    size_t start = sol_piece_table_line_start(pt, line);
    size_t offset = start;
    sol_piece* piece = pt->first;
    
    /* Skip to starting piece */
    while (piece && offset >= piece->length) {
        offset -= piece->length;
        piece = piece->next;
    }
    
    /* Count until newline or end */
    size_t len = 0;
    size_t piece_offset = offset;
    
    while (piece) {
        const char* buf = piece_buffer(pt, piece);
        
        for (size_t i = piece_offset; i < piece->length; i++) {
            if (buf[i] == '\n') {
                return len;
            }
            len++;
        }
        
        piece = piece->next;
        piece_offset = 0;
    }
    
    return len;
}

sol_position sol_piece_table_offset_to_pos(sol_piece_table* pt, size_t offset) {
    sol_position pos = {0, 0};
    if (!pt) return pos;
    
    if (offset > pt->total_length) {
        offset = pt->total_length;
    }
    
    size_t current = 0;
    sol_piece* piece = pt->first;
    
    while (piece && current + piece->length <= offset) {
        const char* buf = piece_buffer(pt, piece);
        for (size_t i = 0; i < piece->length; i++) {
            if (buf[i] == '\n') {
                pos.line++;
                pos.column = 0;
            } else {
                pos.column++;
            }
        }
        current += piece->length;
        piece = piece->next;
    }
    
    /* Process remaining in current piece */
    if (piece) {
        const char* buf = piece_buffer(pt, piece);
        size_t remaining = offset - current;
        for (size_t i = 0; i < remaining; i++) {
            if (buf[i] == '\n') {
                pos.line++;
                pos.column = 0;
            } else {
                pos.column++;
            }
        }
    }
    
    return pos;
}

size_t sol_piece_table_pos_to_offset(sol_piece_table* pt, sol_position pos) {
    if (!pt) return 0;
    
    size_t line_start = sol_piece_table_line_start(pt, pos.line);
    size_t line_len = sol_piece_table_line_length(pt, pos.line);
    
    size_t col = (size_t)pos.column;
    if (col > line_len) col = line_len;
    
    return line_start + col;
}
