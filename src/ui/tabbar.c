/*
 * Sol IDE - Tab Bar Rendering
 */

#include "tui.h"
#include "sol.h"
#include <string.h>
#include <stdio.h>

void sol_tabbar_draw(tui_context* tui, sol_editor* ed, int x, int y, int width) {
    if (!tui || !ed) return;
    
    sol_theme* theme = ed->theme;
    
    /* Set tab bar background */
    tui_set_bg(tui, theme->tabbar_bg);
    tui_set_fg(tui, theme->tabbar_inactive);
    tui_fill(tui, x, y, width, 1, ' ');
    
    int tab_x = x;
    
    for (size_t i = 0; i < sol_array_count(ed->views); i++) {
        sol_view* view = ed->views[i];
        if (!view || !view->buffer) continue;
        
        sol_buffer* buf = view->buffer;
        
        /* Build tab title */
        char title[64];
        snprintf(title, sizeof(title), " %s%s ", 
                 buf->name ? buf->name : "Untitled",
                 buf->modified ? " •" : "");
        
        int tab_width = (int)strlen(title);
        
        /* Check if we have room */
        if (tab_x + tab_width >= x + width) {
            /* Draw overflow indicator */
            tui_set_fg(tui, theme->line_number);  /* Use dimmer color */
            tui_label(tui, x + width - 3, y, "...");
            break;
        }
        
        /* Draw tab */
        bool active = (view == ed->active_view);
        
        if (active) {
            tui_set_bg(tui, theme->tabbar_active);
            tui_set_fg(tui, theme->fg);
        } else {
            tui_set_bg(tui, theme->tabbar_bg);
            tui_set_fg(tui, theme->tabbar_inactive);
        }
        
        tui_label(tui, tab_x, y, title);
        
        /* Tab separator */
        tab_x += tab_width;
        
        if (i < sol_array_count(ed->views) - 1) {
            tui_set_fg(tui, theme->panel_border);
            tui_set_bg(tui, theme->tabbar_bg);
            tui_set_cell(tui, tab_x, y, '|');
            tab_x++;
        }
    }
}
