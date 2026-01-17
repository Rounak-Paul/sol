/*
 * Sol IDE - Dialog Utilities
 */

#include "tui.h"
#include "sol.h"
#include <string.h>
#include <stdio.h>

/* Draw a simple message dialog */
void sol_dialog_message(tui_context* tui, sol_theme* theme, 
                        const char* title, const char* message) {
    if (!tui || !theme || !message) return;
    
    int term_w = tui_get_width(tui);
    int term_h = tui_get_height(tui);
    
    int msg_len = (int)strlen(message);
    int title_len = title ? (int)strlen(title) : 0;
    int width = msg_len + 4;
    if (width < title_len + 4) width = title_len + 4;
    if (width > term_w - 4) width = term_w - 4;
    if (width < 20) width = 20;
    
    int height = 5;
    int x = (term_w - width) / 2;
    int y = (term_h - height) / 2;
    
    /* Draw box */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->widget_fg);
    tui_popup_box(tui, x, y, width, height, title, TUI_BORDER_ROUNDED);
    
    /* Draw message */
    int msg_x = x + (width - msg_len) / 2;
    tui_label(tui, msg_x, y + 2, message);
    
    /* Draw OK button */
    const char* ok = "[ OK ]";
    int ok_x = x + (width - (int)strlen(ok)) / 2;
    tui_set_bg(tui, theme->accent);
    tui_set_fg(tui, theme->widget_bg);
    tui_label(tui, ok_x, y + height - 2, ok);
}

/* Input dialog state */
static char input_buffer[256];
static bool input_active = false;
static void (*input_callback)(const char*, void*) = NULL;
static void* input_userdata = NULL;

void sol_dialog_input(tui_context* tui, sol_theme* theme,
                      const char* title, const char* prompt,
                      void (*callback)(const char*, void*), void* userdata) {
    input_buffer[0] = '\0';
    input_active = true;
    input_callback = callback;
    input_userdata = userdata;
    (void)tui;
    (void)theme;
    (void)title;
    (void)prompt;
}

void sol_dialog_input_draw(tui_context* tui, sol_theme* theme, const char* title) {
    if (!input_active || !tui || !theme) return;
    
    int term_w = tui_get_width(tui);
    int term_h = tui_get_height(tui);
    
    int width = 50;
    int height = 5;
    int x = (term_w - width) / 2;
    int y = (term_h - height) / 2;
    
    /* Draw box */
    tui_set_bg(tui, theme->widget_bg);
    tui_set_fg(tui, theme->widget_fg);
    tui_popup_box(tui, x, y, width, height, title, TUI_BORDER_ROUNDED);
    
    /* Draw input field */
    tui_set_bg(tui, theme->bg);
    tui_set_fg(tui, theme->fg);
    tui_fill(tui, x + 2, y + 2, width - 4, 1, ' ');
    tui_label(tui, x + 2, y + 2, input_buffer);
    
    /* Cursor */
    tui_set_cursor(tui, x + 2 + (int)strlen(input_buffer), y + 2);
    tui_show_cursor(tui, true);
}

bool sol_dialog_input_handle_key(tui_event* event) {
    if (!input_active || !event) return false;
    if (event->type != TUI_EVENT_KEY) return false;
    
    switch (event->key) {
        case TUI_KEY_ESC:
            input_active = false;
            if (input_callback) {
                input_callback(NULL, input_userdata);
            }
            return true;
            
        case TUI_KEY_ENTER:
            input_active = false;
            if (input_callback) {
                input_callback(input_buffer, input_userdata);
            }
            return true;
            
        case TUI_KEY_BACKSPACE:
            {
                size_t len = strlen(input_buffer);
                if (len > 0) {
                    input_buffer[len - 1] = '\0';
                }
            }
            return true;
            
        case TUI_KEY_CHAR:
            if (event->ch >= 32 && event->ch < 127) {
                size_t len = strlen(input_buffer);
                if (len < sizeof(input_buffer) - 1) {
                    input_buffer[len] = (char)event->ch;
                    input_buffer[len + 1] = '\0';
                }
            }
            return true;
            
        default:
            break;
    }
    
    return false;
}

bool sol_dialog_is_active(void) {
    return input_active;
}
