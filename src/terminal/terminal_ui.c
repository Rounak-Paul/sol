/*
 * Sol IDE - Terminal UI
 * Drawing and input handling for terminal emulator
 */

#include "tui.h"
#include "sol.h"

/* Draw terminal - stub for now */
void sol_terminal_draw(tui_context* tui, sol_terminal* term, sol_theme* theme,
                       int x, int y, int w, int h) {
    if (!tui || !term || !theme) return;
    
    /* Draw background */
    tui_set_bg(tui, theme->bg);
    tui_set_fg(tui, theme->fg);
    tui_fill(tui, x, y, w, h, ' ');
    
    /* Draw placeholder text */
    tui_label(tui, x + 1, y, "Terminal (not yet implemented)");
}

/* Handle terminal key input - stub for now */
void sol_terminal_handle_key(sol_terminal* term, tui_event* event) {
    if (!term || !event) return;
    /* TODO: Implement terminal input handling */
    (void)term;
    (void)event;
}

/* Note: sol_terminal_update is implemented in terminal.c */
