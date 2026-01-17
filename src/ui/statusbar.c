/*
 * Sol IDE - Status Bar Rendering
 */

#include "tui.h"
#include "sol.h"
#include "keybind/flow.h"
#include <string.h>
#include <stdio.h>

void sol_statusbar_draw(tui_context* tui, sol_editor* ed, int x, int y, int width) {
    if (!tui || !ed) return;
    
    sol_theme* theme = ed->theme;
    
    /* Set status bar colors */
    tui_set_bg(tui, theme->statusbar_bg);
    tui_set_fg(tui, theme->statusbar_fg);
    
    /* Clear status bar */
    tui_fill(tui, x, y, width, 1, ' ');
    
    char left_text[256] = "";
    char right_text[256] = "";
    char center_text[256] = "";
    
    /* Left side: flow mode indicator or file info */
    int left_offset = 0;
    
    if (ed->flow_active) {
        /* Show flow mode indicator */
        char flow_status[64];
        sol_flow_status_string(ed, flow_status, sizeof(flow_status));
        
        /* Draw flow mode with accent color */
        tui_set_bg(tui, theme->cursor);  /* Accent background */
        tui_set_fg(tui, theme->statusbar_bg);  /* Dark text */
        tui_fill(tui, x, y, (int)strlen(flow_status) + 2, 1, ' ');
        tui_label(tui, x + 1, y, flow_status);
        left_offset = (int)strlen(flow_status) + 2;
        
        /* Reset colors */
        tui_set_bg(tui, theme->statusbar_bg);
        tui_set_fg(tui, theme->statusbar_fg);
    }
    
    if (ed->active_view && ed->active_view->buffer) {
        sol_buffer* buf = ed->active_view->buffer;
        
        snprintf(left_text, sizeof(left_text), " %s%s  %s",
                 buf->name ? buf->name : "Untitled",
                 buf->modified ? " •" : "",
                 buf->language ? buf->language : "");
    }
    
    /* Center: status message */
    if (ed->status_message[0]) {
        uint64_t elapsed = sol_time_ms() - ed->status_time;
        if (elapsed < 5000) {  /* Show for 5 seconds */
            snprintf(center_text, sizeof(center_text), "%s", ed->status_message);
        } else {
            ed->status_message[0] = '\0';
        }
    }
    
    /* Right side: cursor position, encoding, etc */
    if (ed->active_view) {
        sol_view* view = ed->active_view;
        sol_buffer* buf = view->buffer;
        
        const char* eol = "LF";
        if (buf) {
            switch (buf->line_ending) {
                case SOL_LINE_ENDING_CRLF: eol = "CRLF"; break;
                case SOL_LINE_ENDING_CR: eol = "CR"; break;
                default: eol = "LF"; break;
            }
        }
        
        snprintf(right_text, sizeof(right_text), "Ln %d, Col %d  %s  UTF-8  ",
                 view->cursor.pos.line + 1,
                 view->cursor.pos.column + 1,
                 eol);
    }
    
    /* Draw left text */
    tui_label(tui, x + left_offset, y, left_text);
    
    /* Draw center text */
    int center_x = x + (width - (int)strlen(center_text)) / 2;
    if (center_x > x + left_offset + (int)strlen(left_text)) {
        tui_set_fg(tui, theme->cursor);  /* Use cursor color as accent */
        tui_label(tui, center_x, y, center_text);
    }
    
    /* Draw right text */
    int right_x = x + width - (int)strlen(right_text);
    if (right_x > x) {
        tui_set_fg(tui, theme->statusbar_fg);
        tui_label(tui, right_x, y, right_text);
    }
    
    /* Git branch indicator */
    if (ed->git) {
        const char* branch = sol_git_get_branch(ed->git);
        if (branch) {
            char git_text[64];
            snprintf(git_text, sizeof(git_text), "  %s", branch);
            int git_x = x + left_offset + (int)strlen(left_text);
            tui_set_fg(tui, theme->git_modified);
            tui_label(tui, git_x, y, git_text);
        }
    }
}
