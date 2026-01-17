/*
 * Sol IDE - View Implementation
 * 
 * A view is a viewport into a buffer.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

static uint32_t next_view_id = 1;

sol_view* sol_view_create(sol_buffer* buf) {
    sol_view* view = (sol_view*)calloc(1, sizeof(sol_view));
    if (!view) return NULL;
    
    view->id = next_view_id++;
    view->buffer = buf;
    view->line_numbers = true;
    view->word_wrap = false;
    
    /* Initialize cursor at start */
    view->cursor.pos.line = 0;
    view->cursor.pos.column = 0;
    view->cursor.anchor = view->cursor.pos;
    view->cursor.preferred_column = 0;
    
    return view;
}

void sol_view_destroy(sol_view* view) {
    if (!view) return;
    sol_array_free(view->extra_cursors);
    free(view);
}

void sol_view_set_buffer(sol_view* view, sol_buffer* buf) {
    if (!view) return;
    
    view->buffer = buf;
    
    /* Reset view state */
    view->cursor.pos.line = 0;
    view->cursor.pos.column = 0;
    view->cursor.anchor = view->cursor.pos;
    view->scroll_line = 0;
    view->scroll_col = 0;
}

void sol_view_scroll_to_cursor(sol_view* view) {
    if (!view) return;
    
    int visible_lines = view->height - 1;  /* Leave room for status */
    if (visible_lines < 1) visible_lines = 1;
    
    /* Vertical scrolling */
    if (view->cursor.pos.line < view->scroll_line) {
        view->scroll_line = view->cursor.pos.line;
    } else if (view->cursor.pos.line >= view->scroll_line + visible_lines) {
        view->scroll_line = view->cursor.pos.line - visible_lines + 1;
    }
    
    /* Horizontal scrolling */
    int gutter_width = view->line_numbers ? 6 : 0;  /* Line number column */
    int text_width = view->width - gutter_width;
    if (text_width < 1) text_width = 1;
    
    if (view->cursor.pos.column < view->scroll_col) {
        view->scroll_col = view->cursor.pos.column;
    } else if (view->cursor.pos.column >= view->scroll_col + text_width) {
        view->scroll_col = view->cursor.pos.column - text_width + 1;
    }
}

void sol_view_scroll_to_line(sol_view* view, int32_t line) {
    if (!view) return;
    
    int visible_lines = view->height - 1;
    if (visible_lines < 1) visible_lines = 1;
    
    /* Center the line on screen */
    view->scroll_line = line - visible_lines / 2;
    if (view->scroll_line < 0) view->scroll_line = 0;
}

void sol_view_move_cursor(sol_view* view, int dx, int dy) {
    if (!view || !view->buffer) return;
    
    sol_cursor* cursor = &view->cursor;
    int32_t line_count = sol_buffer_line_count(view->buffer);
    
    /* Vertical movement */
    if (dy != 0) {
        cursor->pos.line += dy;
        
        /* Clamp to valid range */
        if (cursor->pos.line < 0) cursor->pos.line = 0;
        if (cursor->pos.line >= line_count) cursor->pos.line = line_count - 1;
        if (cursor->pos.line < 0) cursor->pos.line = 0;
        
        /* Use preferred column */
        int32_t line_len = (int32_t)sol_piece_table_line_length(view->buffer->text, cursor->pos.line);
        cursor->pos.column = cursor->preferred_column;
        if (cursor->pos.column > line_len) cursor->pos.column = line_len;
    }
    
    /* Horizontal movement */
    if (dx != 0) {
        cursor->pos.column += dx;
        
        int32_t line_len = (int32_t)sol_piece_table_line_length(view->buffer->text, cursor->pos.line);
        
        /* Handle wrapping to next/prev line */
        if (cursor->pos.column < 0) {
            if (cursor->pos.line > 0) {
                cursor->pos.line--;
                cursor->pos.column = (int32_t)sol_piece_table_line_length(view->buffer->text, cursor->pos.line);
            } else {
                cursor->pos.column = 0;
            }
        } else if (cursor->pos.column > line_len) {
            if (cursor->pos.line < line_count - 1) {
                cursor->pos.line++;
                cursor->pos.column = 0;
            } else {
                cursor->pos.column = line_len;
            }
        }
        
        cursor->preferred_column = cursor->pos.column;
    }
    
    /* Clear selection when moving without shift */
    cursor->anchor = cursor->pos;
    
    sol_view_scroll_to_cursor(view);
}

void sol_view_move_cursor_to(sol_view* view, sol_position pos) {
    if (!view || !view->buffer) return;
    
    int32_t line_count = sol_buffer_line_count(view->buffer);
    
    /* Clamp position */
    if (pos.line < 0) pos.line = 0;
    if (pos.line >= line_count) pos.line = line_count - 1;
    if (pos.line < 0) pos.line = 0;
    
    int32_t line_len = (int32_t)sol_piece_table_line_length(view->buffer->text, pos.line);
    if (pos.column < 0) pos.column = 0;
    if (pos.column > line_len) pos.column = line_len;
    
    view->cursor.pos = pos;
    view->cursor.anchor = pos;
    view->cursor.preferred_column = pos.column;
    
    sol_view_scroll_to_cursor(view);
}

void sol_view_page_up(sol_view* view) {
    if (!view) return;
    
    int page_size = view->height - 2;
    if (page_size < 1) page_size = 1;
    
    sol_view_move_cursor(view, 0, -page_size);
    view->scroll_line -= page_size;
    if (view->scroll_line < 0) view->scroll_line = 0;
}

void sol_view_page_down(sol_view* view) {
    if (!view || !view->buffer) return;
    
    int page_size = view->height - 2;
    if (page_size < 1) page_size = 1;
    
    sol_view_move_cursor(view, 0, page_size);
    
    int32_t line_count = sol_buffer_line_count(view->buffer);
    view->scroll_line += page_size;
    if (view->scroll_line >= line_count) {
        view->scroll_line = line_count - 1;
    }
    if (view->scroll_line < 0) view->scroll_line = 0;
}

sol_position sol_view_screen_to_buffer(sol_view* view, int screen_x, int screen_y) {
    sol_position pos = {0, 0};
    if (!view || !view->buffer) return pos;
    
    int gutter_width = view->line_numbers ? 5 : 0;
    
    pos.line = view->scroll_line + (screen_y - view->y);
    pos.column = view->scroll_col + (screen_x - view->x - gutter_width);
    
    /* Clamp to valid range */
    int32_t line_count = sol_buffer_line_count(view->buffer);
    if (pos.line < 0) pos.line = 0;
    if (pos.line >= line_count) pos.line = line_count - 1;
    if (pos.line < 0) pos.line = 0;
    
    int32_t line_len = (int32_t)sol_piece_table_line_length(view->buffer->text, pos.line);
    if (pos.column < 0) pos.column = 0;
    if (pos.column > line_len) pos.column = line_len;
    
    return pos;
}

void sol_view_cursor_up(sol_view* view) {
    sol_view_move_cursor(view, 0, -1);
}

void sol_view_cursor_down(sol_view* view) {
    sol_view_move_cursor(view, 0, 1);
}

void sol_view_cursor_left(sol_view* view) {
    sol_view_move_cursor(view, -1, 0);
}

void sol_view_cursor_right(sol_view* view) {
    sol_view_move_cursor(view, 1, 0);
}
