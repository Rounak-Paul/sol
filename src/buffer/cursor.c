/*
 * Sol IDE - Cursor Implementation
 */

#include "sol.h"
#include <stdlib.h>

bool sol_cursor_has_selection(sol_cursor* cursor) {
    if (!cursor) return false;
    return cursor->pos.line != cursor->anchor.line ||
           cursor->pos.column != cursor->anchor.column;
}

sol_range sol_cursor_get_selection(sol_cursor* cursor) {
    sol_range range = {{0, 0}, {0, 0}};
    if (!cursor) return range;
    
    /* Normalize so start <= end */
    if (cursor->pos.line < cursor->anchor.line ||
        (cursor->pos.line == cursor->anchor.line && 
         cursor->pos.column < cursor->anchor.column)) {
        range.start = cursor->pos;
        range.end = cursor->anchor;
    } else {
        range.start = cursor->anchor;
        range.end = cursor->pos;
    }
    
    return range;
}

void sol_cursor_clear_selection(sol_cursor* cursor) {
    if (!cursor) return;
    cursor->anchor = cursor->pos;
}

void sol_cursor_select_all(sol_cursor* cursor, sol_buffer* buf) {
    if (!cursor || !buf) return;
    
    cursor->anchor.line = 0;
    cursor->anchor.column = 0;
    
    int32_t last_line = sol_buffer_line_count(buf) - 1;
    if (last_line < 0) last_line = 0;
    
    cursor->pos.line = last_line;
    cursor->pos.column = (int32_t)sol_piece_table_line_length(buf->text, last_line);
}

void sol_cursor_select_word(sol_cursor* cursor, sol_buffer* buf) {
    if (!cursor || !buf) return;
    
    /* Get current line */
    char line[4096];
    size_t len = sol_buffer_get_line(buf, cursor->pos.line, line, sizeof(line));
    
    if (len == 0 || cursor->pos.column >= (int32_t)len) return;
    
    int col = cursor->pos.column;
    
    /* Find word boundaries */
    int start = col;
    int end = col;
    
    /* Check if we're on a word character */
    if (!((line[col] >= 'a' && line[col] <= 'z') ||
          (line[col] >= 'A' && line[col] <= 'Z') ||
          (line[col] >= '0' && line[col] <= '9') ||
          line[col] == '_')) {
        /* Not on a word, select single character */
        cursor->anchor.line = cursor->pos.line;
        cursor->anchor.column = col;
        cursor->pos.column = col + 1;
        return;
    }
    
    /* Find start of word */
    while (start > 0) {
        char c = line[start - 1];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_')) {
            break;
        }
        start--;
    }
    
    /* Find end of word */
    while (end < (int)len) {
        char c = line[end];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_')) {
            break;
        }
        end++;
    }
    
    cursor->anchor.line = cursor->pos.line;
    cursor->anchor.column = start;
    cursor->pos.column = end;
}

void sol_cursor_select_line(sol_cursor* cursor, sol_buffer* buf) {
    if (!cursor || !buf) return;
    
    cursor->anchor.line = cursor->pos.line;
    cursor->anchor.column = 0;
    
    /* Move to start of next line (or end of buffer) */
    int32_t line_count = sol_buffer_line_count(buf);
    if (cursor->pos.line < line_count - 1) {
        cursor->pos.line++;
        cursor->pos.column = 0;
    } else {
        /* Last line - select to end */
        cursor->pos.column = (int32_t)sol_piece_table_line_length(buf->text, cursor->pos.line);
    }
}
