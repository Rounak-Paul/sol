/*
 * Sol IDE - Editor View Rendering
 */

#include "tui.h"
#include "sol.h"
#include <string.h>
#include <stdio.h>

/* Draw line numbers gutter */
static void draw_line_numbers(tui_context* tui, sol_view* view, sol_theme* theme, 
                               int x, int y, int height) {
    if (!view->line_numbers) return;
    
    int32_t line_count = sol_buffer_line_count(view->buffer);
    
    tui_set_bg(tui, theme->panel_bg);
    
    for (int i = 0; i < height; i++) {
        int32_t line = view->scroll_line + i;
        
        if (line < line_count) {
            /* Line number color */
            if (line == view->cursor.pos.line) {
                tui_set_fg(tui, theme->line_number_active);
            } else {
                tui_set_fg(tui, theme->line_number);
            }
            
            /* Format and draw line number */
            char num[16];
            snprintf(num, sizeof(num), "%4d ", (int)(line + 1));
            tui_label(tui, x, y + i, num);
        } else {
            /* Empty line in gutter */
            tui_set_fg(tui, theme->line_number);
            tui_label(tui, x, y + i, "   ~ ");
        }
    }
}

/* Check if position is within selection */
static bool in_selection(sol_cursor* cursor, int32_t line, int32_t col) {
    if (!sol_cursor_has_selection(cursor)) return false;
    
    sol_range sel = sol_cursor_get_selection(cursor);
    
    if (line < sel.start.line || line > sel.end.line) {
        return false;
    }
    
    if (line == sel.start.line && col < sel.start.column) {
        return false;
    }
    
    if (line == sel.end.line && col >= sel.end.column) {
        return false;
    }
    
    return true;
}

/* Draw the text content */
static void draw_text_content(tui_context* tui, sol_view* view, sol_theme* theme,
                               int x, int y, int width, int height) {
    if (!view->buffer) return;
    
    int gutter_width = view->line_numbers ? 5 : 0;
    int text_x = x + gutter_width;
    int text_width = width - gutter_width;
    
    int32_t line_count = sol_buffer_line_count(view->buffer);
    char line_buf[4096];
    
    for (int row = 0; row < height; row++) {
        int32_t line = view->scroll_line + row;
        int screen_y = y + row;
        
        /* Current line highlight */
        if (line == view->cursor.pos.line && view->focused) {
            tui_set_bg(tui, theme->cursor_line);
        } else {
            tui_set_bg(tui, theme->bg);
        }
        tui_set_fg(tui, theme->fg);
        
        /* Clear the line */
        for (int col = 0; col < text_width; col++) {
            tui_set_cell(tui, text_x + col, screen_y, ' ');
        }
        
        if (line >= line_count) {
            continue;
        }
        
        /* Get line content */
        size_t len = sol_buffer_get_line(view->buffer, line, line_buf, sizeof(line_buf) - 1);
        line_buf[len] = '\0';
        
        /* Draw characters */
        int col = 0;
        const char* p = line_buf;
        int byte_col = 0;
        
        while (*p && col < view->scroll_col + text_width) {
            uint32_t cp;
            int bytes = sol_utf8_decode(p, &cp);
            if (bytes == 0) break;
            
            if (col >= view->scroll_col) {
                int screen_col = col - view->scroll_col;
                
                /* Check for selection */
                if (in_selection(&view->cursor, line, byte_col)) {
                    tui_set_bg(tui, theme->selection);
                } else if (line == view->cursor.pos.line) {
                    tui_set_bg(tui, theme->cursor_line);
                } else {
                    tui_set_bg(tui, theme->bg);
                }
                
                /* TODO: Syntax highlighting would set colors here */
                tui_set_fg(tui, theme->fg);
                
                /* Handle tabs */
                if (cp == '\t') {
                    int tab_width = 4 - (col % 4);
                    for (int t = 0; t < tab_width && screen_col + t < text_width; t++) {
                        tui_set_cell(tui, text_x + screen_col + t, screen_y, ' ');
                    }
                    col += tab_width - 1;
                } else if (cp >= 32) {  /* Printable */
                    tui_set_cell(tui, text_x + screen_col, screen_y, cp);
                }
            }
            
            p += bytes;
            byte_col += bytes;
            col++;
        }
    }
}

/* Draw cursor */
static void draw_cursor(tui_context* tui, sol_view* view, sol_theme* theme,
                        int x, int y, int width, int height) {
    if (!view->focused) return;
    
    int gutter_width = view->line_numbers ? 5 : 0;
    int text_x = x + gutter_width;
    
    int cursor_screen_y = view->cursor.pos.line - view->scroll_line;
    int cursor_screen_x = view->cursor.pos.column - view->scroll_col;
    
    if (cursor_screen_y >= 0 && cursor_screen_y < height &&
        cursor_screen_x >= 0 && cursor_screen_x < width - gutter_width) {
        tui_set_cursor(tui, text_x + cursor_screen_x, y + cursor_screen_y);
        tui_show_cursor(tui, true);
        tui_set_cursor_shape(tui, TUI_CURSOR_BAR);
    }
}

/* Main editor view draw function */
void sol_editor_view_draw(tui_context* tui, sol_view* view, sol_theme* theme) {
    if (!tui || !view || !theme) return;
    
    int x = view->x;
    int y = view->y;
    int width = view->width;
    int height = view->height;
    
    /* Draw components */
    draw_line_numbers(tui, view, theme, x, y, height);
    draw_text_content(tui, view, theme, x, y, width, height);
    
    if (view->focused) {
        draw_cursor(tui, view, theme, x, y, width, height);
    }
}
