/*
 * Sol IDE - Command Palette Implementation
 */

#include "tui.h"
#include "sol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define PALETTE_MAX_VISIBLE 15

/* Sort commands by score */
typedef struct {
    sol_command* cmd;
    int score;
} scored_command;

static int compare_scored(const void* a, const void* b) {
    const scored_command* sa = (const scored_command*)a;
    const scored_command* sb = (const scored_command*)b;
    return sb->score - sa->score;  /* Descending */
}

void sol_palette_update_results(sol_editor* ed) {
    if (!ed) return;
    
    /* Clear previous results */
    sol_array_clear(ed->palette_results);
    
    /* Get all commands */
    sol_array(sol_command*) all_cmds = sol_command_list(ed->commands);
    if (!all_cmds) return;
    
    /* Score and filter */
    scored_command* scored = (scored_command*)malloc(
        sol_array_count(all_cmds) * sizeof(scored_command)
    );
    if (!scored) return;
    
    int scored_count = 0;
    
    for (size_t i = 0; i < sol_array_count(all_cmds); i++) {
        sol_command* cmd = all_cmds[i];
        if (!cmd || !cmd->enabled) continue;
        
        int score = 0;
        bool matches = true;
        
        if (ed->palette_query[0]) {
            /* Fuzzy match against title and id */
            int title_score = 0, id_score = 0;
            
            if (cmd->title) {
                sol_fuzzy_match(ed->palette_query, cmd->title, &title_score);
            }
            if (cmd->id) {
                sol_fuzzy_match(ed->palette_query, cmd->id, &id_score);
            }
            
            score = title_score > id_score ? title_score : id_score;
            matches = (score > 0);
        } else {
            /* No query - show all */
            score = 100;
        }
        
        if (matches) {
            scored[scored_count].cmd = cmd;
            scored[scored_count].score = score;
            scored_count++;
        }
    }
    
    /* Sort by score */
    if (scored_count > 1) {
        qsort(scored, (size_t)scored_count, sizeof(scored_command), compare_scored);
    }
    
    /* Build results array */
    for (int i = 0; i < scored_count; i++) {
        sol_array_push(ed->palette_results, scored[i].cmd);
    }
    
    free(scored);
    
    /* Reset selection */
    ed->palette_selection = 0;
}

void sol_palette_draw(tui_context* tui, sol_editor* ed) {
    if (!tui || !ed || !ed->palette_open) return;
    
    sol_theme* theme = ed->theme;
    
    /* Calculate palette dimensions */
    int term_w = tui_get_width(tui);
    int term_h = tui_get_height(tui);
    
    int palette_w = term_w * 2 / 3;
    if (palette_w < 40) palette_w = 40;
    if (palette_w > 80) palette_w = 80;
    
    int result_count = (int)sol_array_count(ed->palette_results);
    int visible_results = result_count > PALETTE_MAX_VISIBLE ? PALETTE_MAX_VISIBLE : result_count;
    
    int palette_h = 3 + visible_results;  /* Border + input + results */
    
    int palette_x = (term_w - palette_w) / 2;
    int palette_y = term_h / 4;
    
    /* Draw background */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->widget_fg);
    
    /* Draw box */
    tui_popup_box(tui, palette_x, palette_y, palette_w, palette_h, 
                  "Command Palette", TUI_BORDER_ROUNDED);
    
    /* Draw input field */
    int input_y = palette_y + 1;
    tui_set_bg(tui, theme->bg);
    tui_set_fg(tui, theme->fg);
    tui_fill(tui, palette_x + 1, input_y, palette_w - 2, 1, ' ');
    
    /* Draw search icon and query */
    tui_set_fg(tui, theme->accent);
    tui_label(tui, palette_x + 2, input_y, ">");
    tui_set_fg(tui, theme->fg);
    tui_label(tui, palette_x + 4, input_y, ed->palette_query);
    
    /* Cursor */
    int cursor_x = palette_x + 4 + (int)strlen(ed->palette_query);
    tui_set_cursor(tui, cursor_x, input_y);
    tui_show_cursor(tui, true);
    
    /* Draw results */
    int results_y = input_y + 2;
    
    for (int i = 0; i < visible_results; i++) {
        sol_command* cmd = ed->palette_results[i];
        if (!cmd) continue;
        
        bool selected = (i == ed->palette_selection);
        
        if (selected) {
            tui_set_bg(tui, theme->select_bg);
            tui_set_fg(tui, theme->select_fg);
        } else {
            tui_set_bg(tui, theme->widget_bg);
            tui_set_fg(tui, theme->widget_fg);
        }
        
        /* Clear row */
        tui_fill(tui, palette_x + 1, results_y + i, palette_w - 2, 1, ' ');
        
        /* Draw command title */
        if (cmd->title) {
            tui_label(tui, palette_x + 3, results_y + i, cmd->title);
        }
        
        /* Draw command ID (dimmed) */
        if (cmd->id) {
            int id_x = palette_x + palette_w - (int)strlen(cmd->id) - 3;
            if (id_x > palette_x + 20) {
                tui_set_fg(tui, selected ? theme->select_fg : theme->fg_dim);
                tui_label(tui, id_x, results_y + i, cmd->id);
            }
        }
    }
    
    /* Empty state */
    if (result_count == 0) {
        tui_set_bg(tui, theme->widget_bg);
        tui_set_fg(tui, theme->fg_dim);
        tui_label(tui, palette_x + 3, results_y, "No matching commands");
    }
}

void sol_palette_handle_key(sol_editor* ed, tui_event* event) {
    if (!ed || !event || !ed->palette_open) return;
    
    if (event->type != TUI_EVENT_KEY) return;
    
    switch (event->key) {
        case TUI_KEY_ESC:
            ed->palette_open = false;
            ed->palette_query[0] = '\0';
            break;
            
        case TUI_KEY_ENTER:
            if (ed->palette_selection >= 0 && 
                ed->palette_selection < (int)sol_array_count(ed->palette_results)) {
                sol_command* cmd = ed->palette_results[ed->palette_selection];
                if (cmd && cmd->id) {
                    ed->palette_open = false;
                    ed->palette_query[0] = '\0';
                    sol_command_execute(ed->commands, cmd->id, ed);
                }
            }
            break;
            
        case TUI_KEY_UP:
            if (ed->palette_selection > 0) {
                ed->palette_selection--;
            }
            break;
            
        case TUI_KEY_DOWN:
            if (ed->palette_selection < (int)sol_array_count(ed->palette_results) - 1) {
                ed->palette_selection++;
            }
            break;
            
        case TUI_KEY_BACKSPACE:
            {
                size_t len = strlen(ed->palette_query);
                if (len > 0) {
                    ed->palette_query[len - 1] = '\0';
                    sol_palette_update_results(ed);
                }
            }
            break;
            
        case TUI_KEY_CHAR:
            if (event->ch >= 32 && event->ch < 127) {
                size_t len = strlen(ed->palette_query);
                if (len < sizeof(ed->palette_query) - 1) {
                    ed->palette_query[len] = (char)event->ch;
                    ed->palette_query[len + 1] = '\0';
                    sol_palette_update_results(ed);
                }
            }
            break;
            
        default:
            break;
    }
}
