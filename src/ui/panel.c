/*
 * Sol IDE - Panel System Implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

static uint32_t next_panel_id = 1;

sol_panel* sol_panel_create(sol_panel_type type) {
    sol_panel* panel = (sol_panel*)calloc(1, sizeof(sol_panel));
    if (!panel) return NULL;
    
    panel->id = next_panel_id++;
    panel->type = type;
    panel->visible = true;
    panel->split = SOL_SPLIT_NONE;
    panel->split_ratio = 0.5f;
    
    return panel;
}

void sol_panel_destroy(sol_panel* panel) {
    if (!panel) return;
    
    /* Recursively destroy children */
    if (panel->child1) {
        sol_panel_destroy(panel->child1);
    }
    if (panel->child2) {
        sol_panel_destroy(panel->child2);
    }
    
    free(panel->title);
    free(panel);
}

void sol_panel_split(sol_panel* panel, sol_split_type split, sol_panel_type child_type) {
    if (!panel || panel->split != SOL_SPLIT_NONE) return;
    
    panel->split = split;
    
    /* Move current content to child1 */
    panel->child1 = sol_panel_create(panel->type);
    if (panel->child1) {
        panel->child1->parent = panel;
        panel->child1->content = panel->content;
    }
    
    /* Create child2 with new type */
    panel->child2 = sol_panel_create(child_type);
    if (panel->child2) {
        panel->child2->parent = panel;
    }
    
    /* Clear parent content */
    panel->type = SOL_PANEL_CUSTOM;
    memset(&panel->content, 0, sizeof(panel->content));
}

void sol_panel_layout(sol_panel* panel, int x, int y, int width, int height) {
    if (!panel) return;
    
    panel->x = x;
    panel->y = y;
    panel->width = width;
    panel->height = height;
    
    if (panel->split == SOL_SPLIT_NONE) {
        /* Leaf panel - nothing more to do */
        return;
    }
    
    /* Calculate child sizes */
    if (panel->split == SOL_SPLIT_VERTICAL) {
        int split_x = x + (int)(width * panel->split_ratio);
        
        if (panel->child1) {
            sol_panel_layout(panel->child1, x, y, split_x - x, height);
        }
        if (panel->child2) {
            sol_panel_layout(panel->child2, split_x + 1, y, x + width - split_x - 1, height);
        }
    } else {
        int split_y = y + (int)(height * panel->split_ratio);
        
        if (panel->child1) {
            sol_panel_layout(panel->child1, x, y, width, split_y - y);
        }
        if (panel->child2) {
            sol_panel_layout(panel->child2, x, split_y + 1, width, y + height - split_y - 1);
        }
    }
}

sol_panel* sol_panel_find_focused(sol_panel* panel) {
    if (!panel) return NULL;
    
    if (panel->focused) return panel;
    
    sol_panel* found = sol_panel_find_focused(panel->child1);
    if (found) return found;
    
    return sol_panel_find_focused(panel->child2);
}

sol_panel* sol_panel_find_by_type(sol_panel* panel, sol_panel_type type) {
    if (!panel) return NULL;
    
    if (panel->type == type) return panel;
    
    sol_panel* found = sol_panel_find_by_type(panel->child1, type);
    if (found) return found;
    
    return sol_panel_find_by_type(panel->child2, type);
}
