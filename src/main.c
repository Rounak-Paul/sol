/*
 * Sol IDE - Main Application
 * Entry point and main event loop
 */

#define TUI_IMPLEMENTATION
#include "tui.h"
#include "sol.h"
#include "keybind/flow.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <stdarg.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define getcwd _getcwd
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* Global editor instance for signal handlers */
static sol_editor* g_editor = NULL;

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    if (g_editor) {
        g_editor->running = false;
    }
}

/* Initialize editor */
sol_editor* sol_editor_create(void) {
    sol_editor* ed = (sol_editor*)calloc(1, sizeof(sol_editor));
    if (!ed) return NULL;
    
    /* Initialize TUI */
    ed->tui = tui_create();
    if (!ed->tui) {
        free(ed);
        return NULL;
    }
    
    /* Enable mouse capture to prevent scroll events from leaking to outer terminal */
    tui_enable_mouse(ed->tui);
    
    /* Load user configuration from ~/.sol/config.json */
    ed->config = sol_config_load_user();
    
    /* Get theme */
    ed->theme = sol_theme_default_dark();
    
    /* Create command registry */
    ed->commands = sol_command_registry_create();
    if (ed->commands) {
        sol_commands_register_builtin(ed->commands);
    }
    
    /* Create keymap */
    ed->keymap = sol_keymap_create();
    if (ed->keymap) {
        sol_keybind_defaults(ed->keymap);
        /* Load user keybindings from ~/.sol/keybindings.json */
        sol_config_load_keybindings(ed->keymap, ed->config);
    }
    
    /* Apply settings from config */
    ed->sidebar_visible = sol_config_get_bool(ed->config, "sidebar.visible", true);
    ed->sidebar_width = (int)sol_config_get_int(ed->config, "sidebar.width", 30);
    ed->terminal_visible = false;
    ed->terminal_height = (int)sol_config_get_int(ed->config, "terminal.height", 15);
    
    ed->running = true;
    
    return ed;
}

void sol_editor_destroy(sol_editor* ed) {
    if (!ed) return;
    
    /* Close all views */
    for (size_t i = 0; i < sol_array_count(ed->views); i++) {
        sol_view_destroy(ed->views[i]);
    }
    sol_array_free(ed->views);
    
    /* Cleanup subsystems */
    if (ed->plugins) {
        sol_plugin_manager_destroy(ed->plugins);
    }
    if (ed->git) {
        sol_git_close(ed->git);
    }
    
    /* Close terminals */
    for (size_t i = 0; i < sol_array_count(ed->terminals); i++) {
        sol_terminal_destroy(ed->terminals[i]);
    }
    sol_array_free(ed->terminals);
    
    if (ed->filetree) {
        sol_filetree_destroy(ed->filetree);
    }
    if (ed->keymap) {
        sol_keymap_destroy(ed->keymap);
    }
    if (ed->commands) {
        sol_command_registry_destroy(ed->commands);
    }
    if (ed->config) {
        sol_config_destroy(ed->config);
    }
    
    sol_array_free(ed->palette_results);
    
    /* Disable mouse before destroying TUI */
    tui_disable_mouse(ed->tui);
    
    tui_destroy(ed->tui);
    free(ed);
}

/* Open a file */
sol_buffer* sol_editor_open_file(sol_editor* ed, const char* path) {
    if (!ed || !path) return NULL;
    
    /* Check if already open */
    for (size_t i = 0; i < sol_array_count(ed->views); i++) {
        if (ed->views[i]->buffer && ed->views[i]->buffer->filepath &&
            strcmp(ed->views[i]->buffer->filepath, path) == 0) {
            ed->active_view = ed->views[i];
            return ed->views[i]->buffer;
        }
    }
    
    /* Create new buffer */
    sol_buffer* buf = sol_buffer_open(path);
    if (!buf) {
        sol_editor_status(ed, "Failed to open file");
        return NULL;
    }
    
    /* Create view */
    sol_view* view = sol_view_create(buf);
    if (!view) {
        sol_buffer_destroy(buf);
        return NULL;
    }
    
    view->line_numbers = true;
    
    sol_array_push(ed->views, view);
    ed->active_view = view;
    
    sol_editor_status(ed, "Opened: %s", sol_path_basename(path));
    
    return buf;
}

/* Create new file */
sol_buffer* sol_editor_new_file(sol_editor* ed) {
    if (!ed) return NULL;
    
    sol_buffer* buf = sol_buffer_create("untitled");
    sol_view* view = sol_view_create(buf);
    
    sol_array_push(ed->views, view);
    ed->active_view = view;
    
    sol_editor_status(ed, "New file");
    return buf;
}

/* Close view */
void sol_editor_close_view(sol_editor* ed, sol_view* view) {
    if (!ed || !view) return;
    
    /* Find and remove */
    for (size_t i = 0; i < sol_array_count(ed->views); i++) {
        if (ed->views[i] == view) {
            sol_view_destroy(view);
            sol_array_remove(ed->views, i);
            
            /* Update active view */
            if (ed->active_view == view) {
                if (sol_array_count(ed->views) > 0) {
                    ed->active_view = ed->views[i > 0 ? i - 1 : 0];
                } else {
                    ed->active_view = NULL;
                }
            }
            break;
        }
    }
}

/* Status message */
void sol_editor_status(sol_editor* ed, const char* fmt, ...) {
    if (!ed || !fmt) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ed->status_message, sizeof(ed->status_message), fmt, args);
    va_end(args);
    ed->status_time = sol_time_ms();
}

/* Toggle sidebar visibility */
void sol_editor_toggle_sidebar(sol_editor* ed) {
    if (!ed) return;
    ed->sidebar_visible = !ed->sidebar_visible;
}

/* Toggle terminal visibility */
void sol_editor_toggle_terminal(sol_editor* ed) {
    if (!ed) return;
    ed->terminal_visible = !ed->terminal_visible;
}

/* Open command palette */
void sol_editor_open_palette(sol_editor* ed) {
    if (!ed) return;
    /* TODO: Implement command palette popup */
    sol_editor_status(ed, "Command palette not yet implemented");
}

/* Handle keyboard input */
static void handle_input(sol_editor* ed, tui_event* event) {
    if (!ed || !event) return;
    if (event->type != TUI_EVENT_KEY) return;
    
    /* Handle keybindings editor input first if open */
    if (ed->keybind_editor_open) {
        sol_keybindings_handle_key(ed, event);
        return;
    }
    
    /* Handle file picker input first if open */
    if (ed->file_picker_open) {
        sol_file_picker_handle_key(ed, event);
        return;
    }
    
    /* Tab key toggles focus between sidebar and editor */
    if (event->key == TUI_KEY_TAB && !event->ctrl && !event->alt && 
        ed->sidebar_visible && ed->filetree) {
        ed->sidebar_focused = !ed->sidebar_focused;
        return;
    }
    
    /* Handle sidebar input when focused */
    if (ed->sidebar_visible && ed->sidebar_focused && ed->filetree) {
        if (sol_filetree_handle_key(ed, event)) {
            return;
        }
    }
    
    /* Build key chord from event */
    sol_keychord chord = {0};
    chord.mods = 0;
    if (event->ctrl) chord.mods |= SOL_MOD_CTRL;
    if (event->alt) chord.mods |= SOL_MOD_ALT;
    if (event->shift) chord.mods |= SOL_MOD_SHIFT;
    
    /* Convert TUI key to SOL key */
    switch (event->key) {
        case TUI_KEY_CHAR:
            /* For character keys, use the character code (lowercase for letters) */
            if (event->ch >= 'A' && event->ch <= 'Z') {
                chord.key = event->ch + 32;  /* to lowercase */
                chord.mods |= SOL_MOD_SHIFT; /* Mark as shifted */
            } else if (event->ch >= 'a' && event->ch <= 'z') {
                chord.key = event->ch;
            } else if (event->ch >= 1 && event->ch <= 26) {
                /* Ctrl+letter produces ASCII 1-26, convert to 'a'-'z' */
                chord.key = event->ch + 'a' - 1;
                chord.mods |= SOL_MOD_CTRL;
            } else {
                chord.key = event->ch;
            }
            break;
        case TUI_KEY_UP: chord.key = SOL_KEY_UP; break;
        case TUI_KEY_DOWN: chord.key = SOL_KEY_DOWN; break;
        case TUI_KEY_LEFT: chord.key = SOL_KEY_LEFT; break;
        case TUI_KEY_RIGHT: chord.key = SOL_KEY_RIGHT; break;
        case TUI_KEY_ENTER: chord.key = SOL_KEY_ENTER; break;
        case TUI_KEY_TAB: chord.key = SOL_KEY_TAB; break;
        case TUI_KEY_ESC: chord.key = SOL_KEY_ESCAPE; break;
        case TUI_KEY_BACKSPACE: chord.key = SOL_KEY_BACKSPACE; break;
        case TUI_KEY_DELETE: chord.key = SOL_KEY_DELETE; break;
        case TUI_KEY_HOME: chord.key = SOL_KEY_HOME; break;
        case TUI_KEY_END: chord.key = SOL_KEY_END; break;
        case TUI_KEY_PAGEUP: chord.key = SOL_KEY_PAGE_UP; break;
        case TUI_KEY_PAGEDOWN: chord.key = SOL_KEY_PAGE_DOWN; break;
        case TUI_KEY_INSERT: chord.key = SOL_KEY_INSERT; break;
        case TUI_KEY_F1: chord.key = SOL_KEY_F1; break;
        case TUI_KEY_F2: chord.key = SOL_KEY_F2; break;
        case TUI_KEY_F3: chord.key = SOL_KEY_F3; break;
        case TUI_KEY_F4: chord.key = SOL_KEY_F4; break;
        case TUI_KEY_F5: chord.key = SOL_KEY_F5; break;
        case TUI_KEY_F6: chord.key = SOL_KEY_F6; break;
        case TUI_KEY_F7: chord.key = SOL_KEY_F7; break;
        case TUI_KEY_F8: chord.key = SOL_KEY_F8; break;
        case TUI_KEY_F9: chord.key = SOL_KEY_F9; break;
        case TUI_KEY_F10: chord.key = SOL_KEY_F10; break;
        case TUI_KEY_F11: chord.key = SOL_KEY_F11; break;
        case TUI_KEY_F12: chord.key = SOL_KEY_F12; break;
        case TUI_KEY_SPACE: 
            chord.key = ' '; 
            if (event->ctrl) chord.mods |= SOL_MOD_CTRL;
            break;
        default: chord.key = (sol_key)event->key; break;
    }
    
    /* Handle command flow mode (Vim-like leader key system) */
    if (sol_flow_handle_key(ed, &chord)) {
        return;
    }
    
    /* Look up keybinding */
    sol_keysequence seq;
    seq.chords[0] = chord;
    seq.length = 1;
    const char* command = sol_keymap_lookup(ed->keymap, &seq, NULL);
    if (command) {
        sol_editor_status(ed, "Executing: %s", command);
        sol_command_execute(ed->commands, command, ed);
        return;
    }
    
    /* Default text input handling */
    if (ed->active_view) {
        sol_view* view = ed->active_view;
        sol_buffer* buf = view->buffer;
        
        switch (event->key) {
            case TUI_KEY_CHAR:
                if (!event->ctrl && !event->alt) {
                    char text[8];
                    int len = sol_utf8_encode(event->ch, text);
                    text[len] = '\0';
                    sol_buffer_insert(buf, view->cursor.pos, text, (size_t)len);
                    view->cursor.pos.column += 1;
                }
                break;
                
            case TUI_KEY_SPACE:
                sol_buffer_insert(buf, view->cursor.pos, " ", 1);
                view->cursor.pos.column += 1;
                break;
                
            case TUI_KEY_TAB:
                sol_buffer_insert(buf, view->cursor.pos, "    ", 4);
                view->cursor.pos.column += 4;
                break;
                
            case TUI_KEY_ENTER:
                sol_buffer_insert(buf, view->cursor.pos, "\n", 1);
                view->cursor.pos.line++;
                view->cursor.pos.column = 0;
                break;
                
            case TUI_KEY_BACKSPACE:
                if (view->cursor.pos.column > 0 || view->cursor.pos.line > 0) {
                    sol_position prev = view->cursor.pos;
                    if (prev.column > 0) {
                        prev.column--;
                    } else if (prev.line > 0) {
                        prev.line--;
                        prev.column = sol_buffer_line_length(buf, prev.line);
                    }
                    sol_range r = {prev, view->cursor.pos};
                    sol_buffer_delete(buf, r);
                    view->cursor.pos = prev;
                }
                break;
                
            case TUI_KEY_UP:
                sol_view_cursor_up(view);
                break;
            case TUI_KEY_DOWN:
                sol_view_cursor_down(view);
                break;
            case TUI_KEY_LEFT:
                sol_view_cursor_left(view);
                break;
            case TUI_KEY_RIGHT:
                sol_view_cursor_right(view);
                break;
            case TUI_KEY_HOME:
                view->cursor.pos.column = 0;
                break;
            case TUI_KEY_END:
                view->cursor.pos.column = sol_buffer_line_length(buf, view->cursor.pos.line);
                break;
            case TUI_KEY_PAGEUP:
                sol_view_page_up(view);
                break;
            case TUI_KEY_PAGEDOWN:
                sol_view_page_down(view);
                break;
                
            default:
                break;
        }
        
        sol_view_scroll_to_cursor(view);
    }
}

/* Draw the UI */
static void draw_ui(sol_editor* ed) {
    tui_context* tui = ed->tui;
    sol_theme* theme = ed->theme;
    
    int width = tui_get_width(tui);
    int height = tui_get_height(tui);
    
    /* Start frame */
    tui_begin_frame(tui);
    
    /* Clear screen */
    tui_set_bg(tui, theme->bg);
    tui_set_fg(tui, theme->fg);
    tui_clear(tui);
    
    /* Calculate layout */
    int status_height = 1;
    int sidebar_width = ed->sidebar_visible ? ed->sidebar_width : 0;
    
    int editor_x = sidebar_width + 1;
    int editor_y = 0;
    int editor_width = width - sidebar_width - 1;
    int editor_height = height - status_height;
    
    /* Draw sidebar with file tree */
    if (ed->sidebar_visible) {
        /* Draw file tree */
        sol_filetree_draw(tui, ed, 0, 0, sidebar_width, height - status_height);
        
        /* Draw sidebar border */
        tui_set_fg(tui, theme->panel_border);
        for (int y = 0; y < height - status_height; y++) {
            tui_set_cell(tui, sidebar_width, y, 0x2502);  /* │ */
        }
    }
    
    /* Draw editor view or welcome screen */
    if (ed->active_view) {
        ed->active_view->x = editor_x;
        ed->active_view->y = editor_y;
        ed->active_view->width = editor_width;
        ed->active_view->height = editor_height;
        ed->active_view->focused = true;
        
        sol_editor_view_draw(tui, ed->active_view, theme);
    } else {
        /* Welcome screen */
        tui_set_fg(tui, theme->syntax_comment);
        
        const char* welcome[] = {
            "  ____        _ ",
            " / ___|  ___ | |",
            " \\___ \\ / _ \\| |",
            "  ___) | (_) | |",
            " |____/ \\___/|_|",
            "",
            "Welcome to Sol IDE",
            "",
            "Press Ctrl+Space to enter command mode, then:",
            "",
            "  n  New file        s  Save",
            "  o  Open file       w  Close",
            "  O  Open folder     q  Quit",
            "  k  Edit keybindings",
            "",
            "  x  Undo (4x = undo 4 times)",
            "  r  Redo",
            "",
            "Config: ~/.sol/config.json",
            NULL
        };
        
        int center_x = editor_x + editor_width / 2;
        int center_y = editor_y + editor_height / 2 - 8;
        
        for (int i = 0; welcome[i]; i++) {
            int x = center_x - (int)strlen(welcome[i]) / 2;
            tui_label(tui, x, center_y + i, welcome[i]);
        }
    }
    
    /* Draw status bar */
    tui_set_bg(tui, theme->statusbar_bg);
    tui_set_fg(tui, theme->fg);
    tui_fill(tui, 0, height - 1, width, 1, ' ');
    
    /* Status message or file info */
    if (ed->status_message[0] && sol_time_ms() - ed->status_time < 3000) {
        tui_label(tui, 1, height - 1, ed->status_message);
    } else if (ed->active_view && ed->active_view->buffer) {
        char info[256];
        sol_buffer* buf = ed->active_view->buffer;
        snprintf(info, sizeof(info), " %s%s | Ln %d, Col %d",
                 buf->filepath ? buf->filepath : buf->name,
                 buf->modified ? " [+]" : "",
                 ed->active_view->cursor.pos.line + 1,
                 ed->active_view->cursor.pos.column + 1);
        tui_label(tui, 0, height - 1, info);
    } else {
        tui_label(tui, 1, height - 1, "Sol IDE");
    }
    
    /* Draw file picker overlay if open */
    if (ed->file_picker_open) {
        sol_file_picker_draw(tui, ed);
    }
    
    /* Draw keybindings editor overlay if open */
    if (ed->keybind_editor_open) {
        sol_keybindings_draw(tui, ed);
    }
    
    /* Flush to screen */
    tui_end_frame(tui);
}

/* Main loop */
void sol_editor_run(sol_editor* ed) {
    if (!ed) return;
    
    while (ed->running) {
        /* Poll events */
        tui_event event;
        while (tui_poll_event(ed->tui, &event)) {
            if (event.type == TUI_EVENT_RESIZE) {
                /* Handle resize - just redraw */
            } else if (event.type == TUI_EVENT_KEY) {
                handle_input(ed, &event);
            } else if (event.type == TUI_EVENT_MOUSE) {
                int sidebar_width = ed->sidebar_visible ? ed->sidebar_width : 0;
                
                /* Handle mouse click */
                if (event.mouse_button == TUI_MOUSE_LEFT) {
                    /* Check if clicking in sidebar */
                    if (ed->sidebar_visible && event.mouse_x < sidebar_width && ed->filetree) {
                        ed->sidebar_focused = true;
                        
                        /* Calculate which item was clicked */
                        int content_y = 2;  /* After header and folder name */
                        if (event.mouse_y >= content_y) {
                            int clicked_idx = (event.mouse_y - content_y) + 0;  /* Add scroll offset */
                            sol_file_node* node = sol_filetree_get_visible(ed->filetree, clicked_idx);
                            if (node) {
                                ed->filetree->selected = node;
                                /* Single click opens file or toggles folder */
                                if (node->type == SOL_FILE_NODE_DIRECTORY) {
                                    sol_filetree_toggle_expand(ed->filetree, node);
                                } else {
                                    sol_editor_open_file(ed, node->path);
                                    ed->sidebar_focused = false;
                                }
                            }
                        }
                    } else if (ed->active_view) {
                        /* Clicking in editor area */
                        ed->sidebar_focused = false;
                        sol_position pos = sol_view_screen_to_buffer(ed->active_view, 
                                                                      event.mouse_x, event.mouse_y);
                        sol_view_move_cursor_to(ed->active_view, pos);
                    }
                }
                /* Handle mouse scroll */
                else if (event.mouse_button == TUI_MOUSE_WHEEL_UP) {
                    if (ed->active_view) {
                        ed->active_view->scroll_line -= 3;
                        if (ed->active_view->scroll_line < 0) {
                            ed->active_view->scroll_line = 0;
                        }
                    }
                } else if (event.mouse_button == TUI_MOUSE_WHEEL_DOWN) {
                    if (ed->active_view && ed->active_view->buffer) {
                        ed->active_view->scroll_line += 3;
                        int32_t max_scroll = sol_buffer_line_count(ed->active_view->buffer) - 1;
                        if (ed->active_view->scroll_line > max_scroll) {
                            ed->active_view->scroll_line = max_scroll;
                        }
                    }
                }
            }
        }
        
        /* Draw */
        draw_ui(ed);
        
        /* Small delay to prevent busy-looping */
        sleep_ms(16);  /* ~60 fps */
    }
}

/* Open workspace */
void sol_editor_open_workspace(sol_editor* ed, const char* path) {
    if (!ed || !path) return;
    
    /* Create file tree */
    if (ed->filetree) {
        sol_filetree_destroy(ed->filetree);
    }
    ed->filetree = sol_filetree_create(path);
    
    /* Show sidebar when opening a workspace */
    ed->sidebar_visible = true;
    
    /* Create plugin manager */
    ed->plugins = sol_plugin_manager_create(ed);
    
    sol_editor_status(ed, "Opened: %s", path);
}

/* Main entry point */
int main(int argc, char* argv[]) {
    /* Handle --help and --version */
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Sol IDE - A terminal-based code editor\n\n");
            printf("Usage: sol [OPTIONS] [PATH]\n\n");
            printf("Options:\n");
            printf("  -h, --help       Show this help message\n");
            printf("  -v, --version    Show version information\n");
            printf("\nArguments:\n");
            printf("  PATH             File or directory to open\n");
            printf("\nExamples:\n");
            printf("  sol              Open current directory\n");
            printf("  sol .            Open current directory\n");
            printf("  sol myfile.c     Open file\n");
            printf("  sol myproject/   Open directory\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("Sol IDE version 0.1.0\n");
            return 0;
        }
    }
    
    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create editor */
    sol_editor* ed = sol_editor_create();
    if (!ed) {
        fprintf(stderr, "Failed to create editor\n");
        return 1;
    }
    
    g_editor = ed;
    
    /* Open workspace or files from command line */
    if (argc > 1) {
        const char* path = argv[1];
        
        if (sol_path_is_directory(path)) {
            sol_editor_open_workspace(ed, path);
        } else {
            /* Get parent directory as workspace */
            char* dir = sol_path_dirname(path);
            if (dir) {
                sol_editor_open_workspace(ed, dir);
                free(dir);
            }
            
            /* Open the file */
            sol_editor_open_file(ed, path);
        }
    } else {
        /* Open current directory */
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            sol_editor_open_workspace(ed, cwd);
        }
        
        /* Don't create a buffer - show welcome screen instead */
    }
    
    /* Run main loop */
    sol_editor_run(ed);
    
    /* Save config */
    if (ed->config) {
        sol_config_save(ed->config);
    }
    
    /* Cleanup */
    sol_editor_destroy(ed);
    
    return 0;
}
