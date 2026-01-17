/*
 * Sol IDE - File Picker
 * Full filesystem browser for opening files and folders
 */

#include "tui.h"
#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

/* File picker entry */
typedef struct {
    char* name;
    char* path;
    bool is_dir;
} picker_entry;

/* Static state for file picker */
static picker_entry* s_entries = NULL;
static int s_entry_count = 0;
static int s_entry_capacity = 0;
static int s_selected = 0;
static int s_scroll = 0;
static char s_current_path[1024] = {0};

/* Free entries */
static void picker_free_entries(void) {
    for (int i = 0; i < s_entry_count; i++) {
        free(s_entries[i].name);
        free(s_entries[i].path);
    }
    free(s_entries);
    s_entries = NULL;
    s_entry_count = 0;
    s_entry_capacity = 0;
}

/* Add entry */
static void picker_add_entry(const char* name, const char* path, bool is_dir) {
    if (s_entry_count >= s_entry_capacity) {
        int new_cap = s_entry_capacity == 0 ? 64 : s_entry_capacity * 2;
        picker_entry* new_entries = (picker_entry*)realloc(s_entries, 
            (size_t)new_cap * sizeof(picker_entry));
        if (!new_entries) return;
        s_entries = new_entries;
        s_entry_capacity = new_cap;
    }
    
    s_entries[s_entry_count].name = sol_strdup(name);
    s_entries[s_entry_count].path = sol_strdup(path);
    s_entries[s_entry_count].is_dir = is_dir;
    s_entry_count++;
}

/* Compare entries for sorting (directories first, then alphabetical) */
static int compare_entries(const void* a, const void* b) {
    const picker_entry* ea = (const picker_entry*)a;
    const picker_entry* eb = (const picker_entry*)b;
    
    /* Directories first */
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return 1;
    
    /* Then alphabetically (case-insensitive) */
    return strcasecmp(ea->name, eb->name);
}

/* Load directory contents */
static void picker_load_directory(const char* path) {
    if (!path || path[0] == '\0') {
        return;
    }
    
    /* IMPORTANT: Copy path first because it might point to memory we're about to free */
    char path_copy[1024];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    picker_free_entries();
    
    /* Resolve to absolute path */
    char resolved[1024];
    resolved[0] = '\0';
    
#ifdef _WIN32
    if (_fullpath(resolved, path_copy, sizeof(resolved)) == NULL) {
        strncpy(resolved, path_copy, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
#else
    /* Try realpath first, fall back to the path as-is */
    if (realpath(path_copy, resolved) == NULL) {
        /* realpath failed - use path directly */
        strncpy(resolved, path_copy, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
#endif
    
    /* Copy to current path */
    strncpy(s_current_path, resolved, sizeof(s_current_path) - 1);
    s_current_path[sizeof(s_current_path) - 1] = '\0';
    
    /* Add parent directory entry (unless at root) */
    bool at_root = false;
#ifdef _WIN32
    at_root = (strlen(s_current_path) <= 3);  /* e.g., "C:\" */
#else
    at_root = (strcmp(s_current_path, "/") == 0);
#endif
    
    if (!at_root) {
        char* parent = sol_path_dirname(s_current_path);
        if (parent) {
            picker_add_entry("..", parent, true);
            free(parent);
        }
    }
    
    /* List directory contents */
    sol_array(char*) entries = sol_dir_list(s_current_path);
    if (!entries) {
        /* Directory listing failed - keep showing current path with just ".." */
        return;
    }
    
    for (size_t i = 0; i < sol_array_count(entries); i++) {
        char* name = entries[i];
        
        /* Skip . and .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strcmp(name, "./") == 0 || strcmp(name, "../") == 0) {
            free(entries[i]);
            continue;
        }
        
        /* Check if directory (name ends with /) and strip trailing slash */
        size_t len = strlen(name);
        bool is_dir = false;
        if (len > 0 && name[len - 1] == '/') {
            name[len - 1] = '\0';
            len--;
            is_dir = true;
        }
        
        /* Skip empty names */
        if (len == 0) {
            free(entries[i]);
            continue;
        }
        
        /* Build full path */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", s_current_path, name);
        
        /* Double-check if it's a directory */
        if (!is_dir) {
            is_dir = sol_path_is_directory(full_path);
        }
        
        picker_add_entry(name, full_path, is_dir);
        
        free(entries[i]);
    }
    sol_array_free(entries);
    
    /* Sort entries (but keep .. at top) */
    if (s_entry_count > 1) {
        int start = (s_entry_count > 0 && strcmp(s_entries[0].name, "..") == 0) ? 1 : 0;
        if (s_entry_count - start > 1) {
            qsort(s_entries + start, (size_t)(s_entry_count - start), 
                  sizeof(picker_entry), compare_entries);
        }
    }
    
    s_selected = 0;
    s_scroll = 0;
}

/* Open file picker starting from current workspace or home */
void sol_editor_open_file_picker(sol_editor* ed) {
    if (!ed) return;
    
    ed->file_picker_select_folder = false;
    
    /* Start from workspace root or home directory */
    const char* start_path = NULL;
    if (ed->filetree && ed->filetree->root_path) {
        start_path = ed->filetree->root_path;
    } else {
        start_path = getenv("HOME");
        if (!start_path) start_path = ".";
    }
    
    picker_load_directory(start_path);
    ed->file_picker_open = true;
}

/* Open folder picker */
void sol_editor_open_folder_picker(sol_editor* ed) {
    if (!ed) return;
    
    sol_editor_open_file_picker(ed);
    ed->file_picker_select_folder = true;
}

/* Close file picker */
void sol_file_picker_close(sol_editor* ed) {
    if (!ed) return;
    
    ed->file_picker_open = false;
    picker_free_entries();
}

/* Draw file picker */
void sol_file_picker_draw(tui_context* tui, sol_editor* ed) {
    if (!ed || !ed->file_picker_open) return;
    
    sol_theme* theme = ed->theme;
    if (!theme) theme = sol_theme_default_dark();
    
    int term_w = tui_get_width(tui);
    int term_h = tui_get_height(tui);
    
    /* Picker dimensions - centered popup */
    int width = term_w * 2 / 3;
    if (width < 50) width = 50;
    if (width > 90) width = 90;
    
    int height = term_h * 2 / 3;
    if (height < 15) height = 15;
    if (height > 35) height = 35;
    
    int x = (term_w - width) / 2;
    int y = (term_h - height) / 2;
    
    /* Draw popup box */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->widget_fg);
    
    const char* title = ed->file_picker_select_folder ? "Open Folder" : "Open File";
    tui_popup_box(tui, x, y, width, height, title, TUI_BORDER_ROUNDED);
    
    /* Draw current path */
    tui_set_fg(tui, theme->accent);
    char path_display[256];
    int max_path_len = width - 4;
    if ((int)strlen(s_current_path) > max_path_len) {
        snprintf(path_display, sizeof(path_display), "...%s", 
                 s_current_path + strlen(s_current_path) - max_path_len + 3);
    } else {
        strncpy(path_display, s_current_path, sizeof(path_display) - 1);
        path_display[sizeof(path_display) - 1] = '\0';
    }
    tui_label(tui, x + 2, y + 1, path_display);
    
    /* Draw entries */
    int content_y = y + 2;
    int content_height = height - 4;
    
    for (int i = 0; i < content_height && (i + s_scroll) < s_entry_count; i++) {
        int idx = i + s_scroll;
        picker_entry* entry = &s_entries[idx];
        
        int row_y = content_y + i;
        
        /* Highlight selected row */
        if (idx == s_selected) {
            tui_set_bg(tui, theme->selection);
            tui_fill(tui, x + 1, row_y, width - 2, 1, ' ');
        } else {
            tui_set_bg(tui, theme->widget_bg);
        }
        
        /* Set color based on type */
        if (entry->is_dir) {
            tui_set_fg(tui, theme->syntax_function);  /* Blue for directories */
        } else {
            /* Gray out files if selecting folder only */
            if (ed->file_picker_select_folder) {
                tui_set_fg(tui, theme->syntax_comment);
            } else {
                tui_set_fg(tui, theme->fg);
            }
        }
        
        /* Draw icon and name - using Nerd Font icons */
        char line[256];
        const char* icon;
        if (strcmp(entry->name, "..") == 0) {
            icon = " ";  /* nf-md-arrow_up_bold */
        } else if (entry->is_dir) {
            icon = " ";  /* nf-md-folder */
        } else {
            /* Choose icon based on file extension */
            const char* ext = strrchr(entry->name, '.');
            if (ext) {
                if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) {
                    icon = " ";  /* nf-custom-c */
                } else if (strcmp(ext, ".py") == 0) {
                    icon = " ";  /* nf-md-language_python */
                } else if (strcmp(ext, ".js") == 0) {
                    icon = " ";  /* nf-md-language_javascript */
                } else if (strcmp(ext, ".ts") == 0) {
                    icon = " ";  /* nf-md-language_typescript */
                } else if (strcmp(ext, ".rs") == 0) {
                    icon = " ";  /* nf-dev-rust */
                } else if (strcmp(ext, ".go") == 0) {
                    icon = " ";  /* nf-md-language_go */
                } else if (strcmp(ext, ".md") == 0) {
                    icon = " ";  /* nf-md-language_markdown */
                } else if (strcmp(ext, ".json") == 0) {
                    icon = " ";  /* nf-md-code_json */
                } else if (strcmp(ext, ".txt") == 0) {
                    icon = " ";  /* nf-md-file_document */
                } else if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0) {
                    icon = " ";  /* nf-md-console */
                } else if (strcmp(ext, ".git") == 0) {
                    icon = " ";  /* nf-dev-git */
                } else {
                    icon = " ";  /* nf-md-file */
                }
            } else {
                icon = " ";  /* nf-md-file */
            }
        }
        snprintf(line, sizeof(line), "%s %s", icon, entry->name);
        
        /* Truncate if too long */
        if ((int)strlen(line) > width - 4) {
            line[width - 4] = '\0';
        }
        
        tui_label(tui, x + 2, row_y, line);
    }
    
    /* Draw hints at bottom */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->syntax_comment);
    const char* hints = "Up/Down Navigate  Enter Open  Backspace Up  Esc Cancel";
    if (ed->file_picker_select_folder) {
        hints = "Up/Down Navigate  Enter Select  Backspace Up  Esc Cancel";
    }
    int hints_x = x + (width - (int)strlen(hints)) / 2;
    tui_label(tui, hints_x, y + height - 1, hints);
    
    /* Draw scrollbar indicator if needed */
    if (s_entry_count > content_height) {
        tui_set_fg(tui, theme->syntax_comment);
        int scrollbar_y = content_y + (s_scroll * content_height / s_entry_count);
        tui_set_cell(tui, x + width - 2, scrollbar_y, '#');
    }
}

/* Handle file picker input */
bool sol_file_picker_handle_key(sol_editor* ed, tui_event* event) {
    if (!ed || !ed->file_picker_open || !event) return false;
    if (event->type != TUI_EVENT_KEY) return false;
    
    /* Calculate visible height for scrolling */
    int term_h = tui_get_height(ed->tui);
    int height = term_h * 2 / 3;
    if (height < 15) height = 15;
    if (height > 35) height = 35;
    int content_height = height - 4;
    
    switch (event->key) {
        case TUI_KEY_ESC:
            sol_file_picker_close(ed);
            return true;
            
        case TUI_KEY_UP:
            if (s_selected > 0) {
                s_selected--;
                if (s_selected < s_scroll) {
                    s_scroll = s_selected;
                }
            }
            return true;
            
        case TUI_KEY_DOWN:
            if (s_selected < s_entry_count - 1) {
                s_selected++;
                if (s_selected >= s_scroll + content_height) {
                    s_scroll = s_selected - content_height + 1;
                }
            }
            return true;
            
        case TUI_KEY_PAGEUP:
            s_selected -= content_height;
            if (s_selected < 0) s_selected = 0;
            if (s_selected < s_scroll) {
                s_scroll = s_selected;
            }
            return true;
            
        case TUI_KEY_PAGEDOWN:
            s_selected += content_height;
            if (s_selected >= s_entry_count) s_selected = s_entry_count - 1;
            if (s_selected >= s_scroll + content_height) {
                s_scroll = s_selected - content_height + 1;
            }
            return true;
            
        case TUI_KEY_HOME:
            s_selected = 0;
            s_scroll = 0;
            return true;
            
        case TUI_KEY_END:
            s_selected = s_entry_count - 1;
            if (s_selected >= content_height) {
                s_scroll = s_selected - content_height + 1;
            }
            return true;
            
        case TUI_KEY_BACKSPACE:
            /* Go to parent directory */
            {
                char* parent = sol_path_dirname(s_current_path);
                if (parent && strcmp(parent, s_current_path) != 0) {
                    picker_load_directory(parent);
                }
                free(parent);
            }
            return true;
            
        case TUI_KEY_ENTER:
            if (s_selected >= 0 && s_selected < s_entry_count) {
                picker_entry* entry = &s_entries[s_selected];
                
                if (entry->is_dir) {
                    if (ed->file_picker_select_folder && strcmp(entry->name, "..") != 0) {
                        /* Select this folder */
                        sol_editor_open_workspace(ed, entry->path);
                        sol_file_picker_close(ed);
                    } else {
                        /* Navigate into directory */
                        picker_load_directory(entry->path);
                    }
                } else {
                    /* Open the file */
                    if (!ed->file_picker_select_folder) {
                        sol_editor_open_file(ed, entry->path);
                        sol_file_picker_close(ed);
                    }
                }
            }
            return true;
            
        case TUI_KEY_CHAR:
            /* Quick jump to first file starting with typed letter */
            if ((event->ch >= 'a' && event->ch <= 'z') ||
                (event->ch >= 'A' && event->ch <= 'Z')) {
                char lower = (event->ch >= 'A' && event->ch <= 'Z') ? 
                             event->ch + 32 : event->ch;
                for (int i = 0; i < s_entry_count; i++) {
                    char first = s_entries[i].name[0];
                    if (first >= 'A' && first <= 'Z') first += 32;
                    if (first == lower) {
                        s_selected = i;
                        if (s_selected < s_scroll) {
                            s_scroll = s_selected;
                        } else if (s_selected >= s_scroll + content_height) {
                            s_scroll = s_selected - content_height + 1;
                        }
                        break;
                    }
                }
            }
            return true;
            
        default:
            break;
    }
    
    return false;
}
