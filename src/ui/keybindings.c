/*
 * Sol IDE - Keybindings Editor UI
 * 
 * Modal dialog for viewing and editing keybindings.
 */

#include "tui.h"
#include "sol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define KEYBIND_MAX_VISIBLE 15

/* Keybinding entry for display */
typedef struct {
    char key[32];           /* Key combination (e.g., "o", "O", "ctrl+s") */
    char command[64];       /* Command name */
    char description[64];   /* Human-readable description */
    bool is_flow;           /* Flow command vs direct keybinding */
} keybind_entry;

/* Static storage for keybindings list */
static keybind_entry s_keybindings[64];
static int s_keybind_count = 0;

/* Populate keybindings list from config */
static void populate_keybindings(sol_editor* ed) {
    s_keybind_count = 0;
    
    /* Flow commands */
    static const struct {
        const char* key;
        const char* command;
        const char* desc;
    } flow_defaults[] = {
        {"o", "file.open", "Open file"},
        {"O", "file.openFolder", "Open folder"},
        {"n", "file.new", "New file"},
        {"s", "file.save", "Save file"},
        {"S", "file.saveAll", "Save all"},
        {"w", "view.close", "Close buffer"},
        {"x", "edit.undo", "Undo"},
        {"X", "edit.undoAll", "Undo all"},
        {"r", "edit.redo", "Redo"},
        {"R", "edit.redoAll", "Redo all"},
        {"c", "edit.copy", "Copy"},
        {"v", "edit.paste", "Paste"},
        {"d", "edit.cut", "Cut"},
        {"f", "find.open", "Find"},
        {"g", "goto.line", "Go to line"},
        {"e", "view.focusSidebar", "Focus sidebar"},
        {"p", "view.commandPalette", "Command palette"},
        {"b", "view.toggleSidebar", "Toggle sidebar"},
        {"t", "view.toggleTerminal", "Toggle terminal"},
        {"q", "app.quit", "Quit"},
        {"k", "view.keybindings", "Edit keybindings"},
        {NULL, NULL, NULL}
    };
    
    /* Add flow commands */
    for (int i = 0; flow_defaults[i].key && s_keybind_count < 64; i++) {
        keybind_entry* e = &s_keybindings[s_keybind_count++];
        snprintf(e->key, sizeof(e->key), "%s", flow_defaults[i].key);
        snprintf(e->command, sizeof(e->command), "%s", flow_defaults[i].command);
        snprintf(e->description, sizeof(e->description), "%s", flow_defaults[i].desc);
        e->is_flow = true;
    }
    
    /* Direct keybindings */
    static const struct {
        const char* key;
        const char* command;
        const char* desc;
    } direct_defaults[] = {
        {"ctrl+q", "app.quit", "Quit directly"},
        {"ctrl+s", "file.save", "Save directly"},
        {"ctrl+z", "edit.undo", "Undo directly"},
        {"ctrl+y", "edit.redo", "Redo directly"},
        {NULL, NULL, NULL}
    };
    
    /* Add direct keybindings */
    for (int i = 0; direct_defaults[i].key && s_keybind_count < 64; i++) {
        keybind_entry* e = &s_keybindings[s_keybind_count++];
        snprintf(e->key, sizeof(e->key), "%s", direct_defaults[i].key);
        snprintf(e->command, sizeof(e->command), "%s", direct_defaults[i].command);
        snprintf(e->description, sizeof(e->description), "%s", direct_defaults[i].desc);
        e->is_flow = false;
    }
    
    (void)ed;
}

void sol_keybindings_open(sol_editor* ed) {
    if (!ed) return;
    ed->keybind_editor_open = true;
    ed->keybind_selection = 0;
    ed->keybind_scroll = 0;
    ed->keybind_rebinding = false;
    populate_keybindings(ed);
}

void sol_keybindings_close(sol_editor* ed) {
    if (!ed) return;
    ed->keybind_editor_open = false;
    ed->keybind_rebinding = false;
}

void sol_keybindings_draw(tui_context* tui, sol_editor* ed) {
    if (!tui || !ed || !ed->keybind_editor_open) return;
    
    sol_theme* theme = ed->theme;
    
    int term_w = tui_get_width(tui);
    int term_h = tui_get_height(tui);
    
    /* Calculate dialog dimensions */
    int dialog_w = 70;
    if (dialog_w > term_w - 4) dialog_w = term_w - 4;
    
    int visible_count = s_keybind_count > KEYBIND_MAX_VISIBLE ? KEYBIND_MAX_VISIBLE : s_keybind_count;
    int dialog_h = visible_count + 5;  /* Title + header + items + footer + border */
    
    int dialog_x = (term_w - dialog_w) / 2;
    int dialog_y = (term_h - dialog_h) / 2;
    
    /* Draw background box */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->widget_fg);
    tui_popup_box(tui, dialog_x, dialog_y, dialog_w, dialog_h, 
                  "Keybindings", TUI_BORDER_ROUNDED);
    
    /* Header row */
    int header_y = dialog_y + 1;
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->accent);
    tui_label(tui, dialog_x + 3, header_y, "Key");
    tui_label(tui, dialog_x + 15, header_y, "Command");
    tui_label(tui, dialog_x + 40, header_y, "Description");
    
    /* Draw separator */
    tui_set_fg(tui, theme->fg_dim);
    for (int x = dialog_x + 2; x < dialog_x + dialog_w - 2; x++) {
        tui_label(tui, x, header_y + 1, "─");
    }
    
    /* Draw keybindings list */
    int list_y = header_y + 2;
    int start_idx = ed->keybind_scroll;
    int end_idx = start_idx + visible_count;
    if (end_idx > s_keybind_count) end_idx = s_keybind_count;
    
    for (int i = start_idx; i < end_idx; i++) {
        keybind_entry* e = &s_keybindings[i];
        int row_y = list_y + (i - start_idx);
        bool selected = (i == ed->keybind_selection);
        
        if (selected) {
            tui_set_bg(tui, theme->select_bg);
            tui_set_fg(tui, theme->select_fg);
            /* Clear row */
            tui_fill(tui, dialog_x + 1, row_y, dialog_w - 2, 1, ' ');
        } else {
            tui_set_bg(tui, theme->widget_bg);
            tui_set_fg(tui, theme->widget_fg);
        }
        
        /* Key column */
        if (e->is_flow) {
            tui_set_fg(tui, selected ? theme->select_fg : theme->syntax_keyword);
        }
        tui_label(tui, dialog_x + 3, row_y, e->key);
        
        /* Command column */
        tui_set_fg(tui, selected ? theme->select_fg : theme->fg);
        char cmd_short[25];
        strncpy(cmd_short, e->command, 24);
        cmd_short[24] = '\0';
        tui_label(tui, dialog_x + 15, row_y, cmd_short);
        
        /* Description column */
        tui_set_fg(tui, selected ? theme->select_fg : theme->fg_dim);
        char desc_short[25];
        strncpy(desc_short, e->description, 24);
        desc_short[24] = '\0';
        tui_label(tui, dialog_x + 40, row_y, desc_short);
    }
    
    /* Footer with instructions */
    int footer_y = dialog_y + dialog_h - 2;
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->fg_dim);
    
    if (ed->keybind_rebinding) {
        tui_set_fg(tui, theme->syntax_string);
        tui_label(tui, dialog_x + 3, footer_y, "Press new key... (ESC to cancel)");
    } else {
        tui_label(tui, dialog_x + 3, footer_y, "↑↓ Navigate  Enter Rebind  ESC Close  ? Help");
    }
    
    /* Scroll indicator if needed */
    if (s_keybind_count > visible_count) {
        tui_set_fg(tui, theme->fg_dim);
        if (ed->keybind_scroll > 0) {
            tui_label(tui, dialog_x + dialog_w - 4, list_y, "▲");
        }
        if (end_idx < s_keybind_count) {
            tui_label(tui, dialog_x + dialog_w - 4, list_y + visible_count - 1, "▼");
        }
    }
    
    tui_show_cursor(tui, false);
}

/* Format a keychord as a string */
static void format_keychord(sol_keychord* chord, char* buf, size_t len) {
    buf[0] = '\0';
    
    if (chord->mods & SOL_MOD_CTRL) {
        strncat(buf, "ctrl+", len - strlen(buf) - 1);
    }
    if (chord->mods & SOL_MOD_ALT) {
        strncat(buf, "alt+", len - strlen(buf) - 1);
    }
    if (chord->mods & SOL_MOD_SHIFT) {
        strncat(buf, "shift+", len - strlen(buf) - 1);
    }
    
    int key = (int)chord->key;
    
    /* Handle special keys */
    if (key >= 'a' && key <= 'z') {
        char k[2] = {(char)key, '\0'};
        strncat(buf, k, len - strlen(buf) - 1);
    } else if (key >= 'A' && key <= 'Z') {
        /* Uppercase letter - remove shift+ prefix if added, use capital */
        char* shift = strstr(buf, "shift+");
        if (shift) {
            /* Remove shift+ */
            memmove(shift, shift + 6, strlen(shift + 6) + 1);
        }
        char k[2] = {(char)key, '\0'};
        strncat(buf, k, len - strlen(buf) - 1);
    } else if (key == ' ') {
        strncat(buf, "space", len - strlen(buf) - 1);
    } else if (key == (int)TUI_KEY_ENTER) {
        strncat(buf, "enter", len - strlen(buf) - 1);
    } else if (key == (int)TUI_KEY_TAB) {
        strncat(buf, "tab", len - strlen(buf) - 1);
    } else if (key >= '0' && key <= '9') {
        char k[2] = {(char)key, '\0'};
        strncat(buf, k, len - strlen(buf) - 1);
    } else if (key > 0 && key < 128) {
        char k[2] = {(char)key, '\0'};
        strncat(buf, k, len - strlen(buf) - 1);
    }
}

bool sol_keybindings_handle_key(sol_editor* ed, tui_event* event) {
    if (!ed || !event || !ed->keybind_editor_open) return false;
    if (event->type != TUI_EVENT_KEY) return false;
    
    /* Rebinding mode */
    if (ed->keybind_rebinding) {
        if (event->key == TUI_KEY_ESC) {
            ed->keybind_rebinding = false;
            return true;
        }
        
        /* Capture the new key */
        sol_keychord chord;
        chord.key = (sol_key)event->key;
        chord.mods = 0;
        if (event->ctrl) chord.mods |= SOL_MOD_CTRL;
        if (event->alt) chord.mods |= SOL_MOD_ALT;
        if (event->shift) chord.mods |= SOL_MOD_SHIFT;
        
        /* Format the new key */
        char new_key[32];
        format_keychord(&chord, new_key, sizeof(new_key));
        
        if (new_key[0] && ed->keybind_selection < s_keybind_count) {
            keybind_entry* e = &s_keybindings[ed->keybind_selection];
            strncpy(e->key, new_key, sizeof(e->key) - 1);
            e->key[sizeof(e->key) - 1] = '\0';
            
            /* TODO: Save to keybindings.json */
            sol_editor_status(ed, "Rebound to '%s' (restart to apply)", new_key);
        }
        
        ed->keybind_rebinding = false;
        return true;
    }
    
    /* Normal navigation */
    int key = (int)event->key;
    
    if (key == TUI_KEY_ESC) {
        sol_keybindings_close(ed);
        return true;
    }
    
    if (key == TUI_KEY_UP || key == 'k') {
        if (ed->keybind_selection > 0) {
            ed->keybind_selection--;
            /* Scroll if needed */
            if (ed->keybind_selection < ed->keybind_scroll) {
                ed->keybind_scroll = ed->keybind_selection;
            }
        }
        return true;
    }
    
    if (key == TUI_KEY_DOWN || key == 'j') {
        if (ed->keybind_selection < s_keybind_count - 1) {
            ed->keybind_selection++;
            /* Scroll if needed */
            if (ed->keybind_selection >= ed->keybind_scroll + KEYBIND_MAX_VISIBLE) {
                ed->keybind_scroll = ed->keybind_selection - KEYBIND_MAX_VISIBLE + 1;
            }
        }
        return true;
    }
    
    if (key == TUI_KEY_ENTER) {
        /* Start rebinding */
        ed->keybind_rebinding = true;
        return true;
    }
    
    if (key == TUI_KEY_HOME) {
        ed->keybind_selection = 0;
        ed->keybind_scroll = 0;
        return true;
    }
    
    if (key == TUI_KEY_END) {
        ed->keybind_selection = s_keybind_count - 1;
        if (s_keybind_count > KEYBIND_MAX_VISIBLE) {
            ed->keybind_scroll = s_keybind_count - KEYBIND_MAX_VISIBLE;
        }
        return true;
    }
    
    if (key == '?') {
        sol_editor_status(ed, "Use arrow keys to navigate, Enter to rebind, ESC to close");
        return true;
    }
    
    return false;
}
