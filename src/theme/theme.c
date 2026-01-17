/*
 * Sol IDE - Theme System
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Dark theme (default) */
static sol_theme dark_theme = {
    .name = "Sol Dark",
    
    /* Base colors */
    .bg = 0x1E1E2E,
    .fg = 0xCDD6F4,
    .fg_dim = 0x6C7086,
    .accent = 0x89B4FA,
    .selection = 0x45475A,
    .cursor = 0xF5E0DC,
    .cursor_line = 0x313244,
    
    /* Panel colors */
    .panel_bg = 0x181825,
    .panel_border = 0x45475A,
    
    /* Widget colors */
    .widget_bg = 0x313244,
    .widget_fg = 0xCDD6F4,
    .select_bg = 0x89B4FA,
    .select_fg = 0x1E1E2E,
    
    /* Status bar */
    .statusbar_bg = 0x181825,
    .statusbar_fg = 0xBAC2DE,
    
    /* Tab bar */
    .tabbar_bg = 0x181825,
    .tabbar_active = 0x1E1E2E,
    .tabbar_inactive = 0x6C7086,
    
    /* Line numbers */
    .line_number = 0x6C7086,
    .line_number_active = 0xBAC2DE,
    
    /* Syntax highlighting */
    .syntax_keyword = 0xCBA6F7,
    .syntax_type = 0xF9E2AF,
    .syntax_function = 0x89B4FA,
    .syntax_variable = 0xF38BA8,
    .syntax_string = 0xA6E3A1,
    .syntax_number = 0xFAB387,
    .syntax_comment = 0x6C7086,
    .syntax_operator = 0x89DCEB,
    .syntax_punctuation = 0xBAC2DE,
    .syntax_preprocessor = 0xF5C2E7,
    .syntax_constant = 0xFAB387,
    .syntax_escape = 0xF2CDCD,
    
    /* Git colors */
    .git_added = 0xA6E3A1,
    .git_modified = 0xF9E2AF,
    .git_deleted = 0xF38BA8,
    .git_conflict = 0xFAB387,
    
    /* Diagnostic colors */
    .error = 0xF38BA8,
    .warning = 0xF9E2AF,
    .info = 0x89B4FA,
    .hint = 0x94E2D5,
};

/* Light theme */
static sol_theme light_theme = {
    .name = "Sol Light",
    
    /* Base colors */
    .bg = 0xEFF1F5,
    .fg = 0x4C4F69,
    .fg_dim = 0x9CA0B0,
    .accent = 0x1E66F5,
    .selection = 0xACB0BE,
    .cursor = 0xDC8A78,
    .cursor_line = 0xE6E9EF,
    
    /* Panel colors */
    .panel_bg = 0xE6E9EF,
    .panel_border = 0xBCC0CC,
    
    /* Widget colors */
    .widget_bg = 0xCCD0DA,
    .widget_fg = 0x4C4F69,
    .select_bg = 0x1E66F5,
    .select_fg = 0xEFF1F5,
    
    /* Status bar */
    .statusbar_bg = 0xE6E9EF,
    .statusbar_fg = 0x5C5F77,
    
    /* Tab bar */
    .tabbar_bg = 0xE6E9EF,
    .tabbar_active = 0xEFF1F5,
    .tabbar_inactive = 0x9CA0B0,
    
    /* Line numbers */
    .line_number = 0x9CA0B0,
    .line_number_active = 0x5C5F77,
    
    /* Syntax highlighting */
    .syntax_keyword = 0x8839EF,
    .syntax_type = 0xDF8E1D,
    .syntax_function = 0x1E66F5,
    .syntax_variable = 0xD20F39,
    .syntax_string = 0x40A02B,
    .syntax_number = 0xFE640B,
    .syntax_comment = 0x9CA0B0,
    .syntax_operator = 0x179299,
    .syntax_punctuation = 0x5C5F77,
    .syntax_preprocessor = 0xEA76CB,
    .syntax_constant = 0xFE640B,
    .syntax_escape = 0xDD7878,
    
    /* Git colors */
    .git_added = 0x40A02B,
    .git_modified = 0xDF8E1D,
    .git_deleted = 0xD20F39,
    .git_conflict = 0xFE640B,
    
    /* Diagnostic colors */
    .error = 0xD20F39,
    .warning = 0xDF8E1D,
    .info = 0x1E66F5,
    .hint = 0x179299,
};

sol_theme* sol_theme_default_dark(void) {
    return &dark_theme;
}

sol_theme* sol_theme_default_light(void) {
    return &light_theme;
}

sol_theme* sol_theme_clone(sol_theme* theme) {
    if (!theme) return NULL;
    
    sol_theme* copy = (sol_theme*)malloc(sizeof(sol_theme));
    if (!copy) return NULL;
    
    memcpy(copy, theme, sizeof(sol_theme));
    copy->name = sol_strdup(theme->name);
    
    return copy;
}

void sol_theme_free(sol_theme* theme) {
    if (!theme) return;
    if (theme == &dark_theme || theme == &light_theme) return;
    
    free((char*)theme->name);
    free(theme);
}
