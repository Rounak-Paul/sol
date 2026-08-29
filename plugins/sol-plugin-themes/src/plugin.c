// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* LocalDocsMD-inspired semantic themes for Sol. */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "sol_theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define THEME_CSS_CAPACITY 8192u

typedef struct ThemePalette {
    const char *id;
    const char *name;
    const char *background;
    const char *surface;
    const char *elevated;
    const char *text;
    const char *secondary;
    const char *muted;
    const char *primary;
    const char *accent;
    const char *danger;
    const char *success;
    const char *warning;
    uint32_t primary_rgb;
    uint32_t accent_rgb;
    bool light;
} ThemePalette;

typedef struct ThemeCssBuilder {
    char *data;
    size_t capacity;
    size_t length;
    bool valid;
} ThemeCssBuilder;

static const ThemePalette k_themes[] = {
    { "com.sol.theme.midnight", "Midnight", "#06080f", "#0a0e1a", "#0e1424", "#e8eef8", "#b8c8e8", "#7890b8", "#60a5fa", "#a78bfa", "#f87171", "#4ade80", "#fbbf24", 0x60a5fa, 0xa78bfa, false },
    { "com.sol.theme.daylight", "Daylight", "#fdf8f0", "#fffcf7", "#ffffff", "#2c1a0a", "#4a2e12", "#7a5535", "#c2610a", "#d97706", "#b91c1c", "#166534", "#92400e", 0xc2610a, 0xd97706, true },
    { "com.sol.theme.catppuccin", "Catppuccin", "#1e1e2e", "#181825", "#11111b", "#cdd6f4", "#bac2de", "#a6adc8", "#cba6f7", "#94e2d5", "#f38ba8", "#a6e3a1", "#f9e2af", 0xcba6f7, 0x94e2d5, false },
    { "com.sol.theme.obsidian", "Obsidian", "#1a1625", "#242038", "#2e2a40", "#dcddde", "#b5b6bb", "#8e8ea0", "#7c6f9e", "#a8a4c0", "#e06c75", "#98c379", "#e5c07b", 0x7c6f9e, 0xa8a4c0, false },
    { "com.sol.theme.oled", "OLED", "#000000", "#0a0a0a", "#111111", "#e8e8e8", "#b0b0b0", "#808080", "#00e5ff", "#00e5ff", "#ff5555", "#50fa7b", "#ffb86c", 0x00e5ff, 0x00e5ff, false },
    { "com.sol.theme.dracula", "Dracula", "#282a36", "#21222c", "#1e1f29", "#f8f8f2", "#e0dff5", "#6272a4", "#bd93f9", "#50fa7b", "#ff5555", "#50fa7b", "#ffb86c", 0xbd93f9, 0x50fa7b, false },
    { "com.sol.theme.nord", "Nord", "#2e3440", "#3b4252", "#434c5e", "#eceff4", "#e5e9f0", "#9099aa", "#88c0d0", "#81a1c1", "#bf616a", "#a3be8c", "#ebcb8b", 0x88c0d0, 0x81a1c1, false },
    { "com.sol.theme.gruvbox", "Gruvbox", "#282828", "#3c3836", "#504945", "#ebdbb2", "#d5c4a1", "#928374", "#fabd2f", "#d65d0e", "#cc241d", "#98971a", "#d79921", 0xfabd2f, 0xd65d0e, false },
    { "com.sol.theme.solarized-light", "Solarized Light", "#fdf6e3", "#eee8d5", "#e8e2d0", "#657b83", "#586e75", "#839496", "#268bd2", "#2aa198", "#dc322f", "#859900", "#b58900", 0x268bd2, 0x2aa198, true },
    { "com.sol.theme.solarized-dark", "Solarized Dark", "#002b36", "#073642", "#0a4050", "#839496", "#93a1a1", "#657b83", "#268bd2", "#2aa198", "#dc322f", "#859900", "#b58900", 0x268bd2, 0x2aa198, false },
    { "com.sol.theme.tokyo-night", "Tokyo Night", "#1a1b26", "#24283b", "#292e42", "#c0caf5", "#a9b1d6", "#565f89", "#7aa2f7", "#bb9af7", "#f7768e", "#9ece6a", "#e0af68", 0x7aa2f7, 0xbb9af7, false },
    { "com.sol.theme.monokai", "Monokai", "#272822", "#1e1f1a", "#3e3d32", "#f8f8f2", "#d8d8d2", "#75715e", "#a6e22e", "#66d9ef", "#f92672", "#a6e22e", "#e6db74", 0xa6e22e, 0x66d9ef, false },
    { "com.sol.theme.github-light", "GitHub Light", "#ffffff", "#f6f8fa", "#eaeef2", "#1f2328", "#424a53", "#656d76", "#0969da", "#8250df", "#cf222e", "#1a7f37", "#9a6700", 0x0969da, 0x8250df, true },
    { "com.sol.theme.github-dark", "GitHub Dark", "#0d1117", "#161b22", "#21262d", "#e6edf3", "#c9d1d9", "#8b949e", "#58a6ff", "#bc8cff", "#ff7b72", "#7ee787", "#d29922", 0x58a6ff, 0xbc8cff, false },
    { "com.sol.theme.forest", "Forest", "#0f1c0f", "#162416", "#1c2e1c", "#dcedc8", "#bdd7aa", "#7ea87e", "#4caf50", "#8bc34a", "#ef5350", "#66bb6a", "#d4a72c", 0x4caf50, 0x8bc34a, false },
    { "com.sol.theme.rose", "Rose", "#1a0a0e", "#220d12", "#2e1018", "#ffe4e6", "#fecdd3", "#be7b86", "#f43f5e", "#fb7185", "#fb7185", "#4ade80", "#fbbf24", 0xf43f5e, 0xfb7185, false },
    { "com.sol.theme.sunset", "Sunset", "#18100a", "#201408", "#2a1c0e", "#fff7ed", "#fed7aa", "#c07040", "#f97316", "#fbbf24", "#fb7185", "#84cc16", "#fbbf24", 0xf97316, 0xfbbf24, false },
    { "com.sol.theme.ocean", "Ocean", "#061820", "#0a2233", "#0e2d42", "#cffafe", "#a5f3fc", "#4e9aaa", "#06b6d4", "#22d3ee", "#fb7185", "#34d399", "#fbbf24", 0x06b6d4, 0x22d3ee, false },
    { "com.sol.theme.aurora", "Aurora", "#0c0a1a", "#13102a", "#1a1636", "#ede9fe", "#ddd6fe", "#7c6aa6", "#a78bfa", "#34d399", "#fb7185", "#34d399", "#fbbf24", 0xa78bfa, 0x34d399, false },
    { "com.sol.theme.slate", "Slate", "#0f172a", "#1e293b", "#263244", "#e2e8f0", "#cbd5e1", "#64748b", "#94a3b8", "#38bdf8", "#fb7185", "#4ade80", "#fbbf24", 0x94a3b8, 0x38bdf8, false },
    { "com.sol.theme.copper", "Copper", "#1a1208", "#221608", "#2e1e0c", "#fef3e2", "#efd0a8", "#a07840", "#b87333", "#e0a060", "#e85d4a", "#86a65c", "#d9a441", 0xb87333, 0xe0a060, false },
    { "com.sol.theme.sakura", "Sakura", "#fdf2f8", "#fce7f3", "#fde8f4", "#4a1535", "#702552", "#a0608a", "#e879a0", "#c084fc", "#dc2626", "#16805b", "#b45309", 0xe879a0, 0xc084fc, true },
    { "com.sol.theme.terminal", "Terminal", "#000000", "#0a0a0a", "#111111", "#00ff41", "#00c832", "#007a1e", "#00ff41", "#00d9ff", "#ff4040", "#00ff41", "#ffd000", 0x00ff41, 0x00d9ff, false },
    { "com.sol.theme.coffee", "Coffee", "#1a1410", "#241e18", "#2e2620", "#f5e6d0", "#ddc7a8", "#987850", "#c8924a", "#e0b070", "#d06050", "#8fa85c", "#d5a84c", 0xc8924a, 0xe0b070, false },
    { "com.sol.theme.arctic", "Arctic", "#f0f6fc", "#e4eef8", "#d8e6f4", "#0d2340", "#23466c", "#4a7098", "#5e9fd8", "#7c8fd8", "#c43d4d", "#327a57", "#986b10", 0x5e9fd8, 0x7c8fd8, true },
    { "com.sol.theme.hc-light", "High Contrast Light", "#ffffff", "#ffffff", "#f0f0f0", "#000000", "#1a1a1a", "#595959", "#0050e6", "#0050e6", "#cc0000", "#006600", "#7a4f00", 0x0050e6, 0x0050e6, true },
    { "com.sol.theme.hc-dark", "High Contrast Dark", "#000000", "#0d0d0d", "#1a1a1a", "#ffffff", "#e0e0e0", "#b0b0b0", "#ffff00", "#00ffff", "#ff6666", "#66ff66", "#ffcc00", 0xffff00, 0x00ffff, false },
};

/* Append formatted CSS while preserving a single truncation state. */
static void css_append(ThemeCssBuilder *builder, const char *format, ...)
{
    if (!builder || !builder->valid || builder->length >= builder->capacity) return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(builder->data + builder->length,
                                  builder->capacity - builder->length,
                                  format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= builder->capacity - builder->length) {
        builder->valid = false;
        return;
    }
    builder->length += (size_t)written;
}

/* Convert a compile-time #rrggbb palette color to CSS rgba notation. */
static bool color_with_alpha(const char *hex, float alpha, char out[32])
{
    unsigned int r = 0u, g = 0u, b = 0u;
    if (!hex || strlen(hex) != 7u || hex[0] != '#' ||
        sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) != 3)
        return false;
    return snprintf(out, 32, "rgba(%u,%u,%u,%.2f)", r, g, b, alpha) > 0;
}

/* Return a compile-time #rrggbb palette color as packed RGB. */
static uint32_t packed_color(const char *hex)
{
    unsigned int r = 0u, g = 0u, b = 0u;
    if (!hex || strlen(hex) != 7u || hex[0] != '#' ||
        sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) != 3)
        return 0u;
    return (uint32_t)((r << 16u) | (g << 8u) | b);
}

/* Build a complete semantic override for one palette. */
static bool build_theme_css(const ThemePalette *theme, char *out, size_t capacity)
{
    if (!theme || !out || capacity == 0u) return false;
    char chrome[32], panel[32], editor[32], raised[32], hover[32], selected[32];
    const float surface_alpha = theme->light ? 0.74f : 0.64f;
    const float editor_alpha = theme->light ? 0.68f : 0.54f;
    if (!color_with_alpha(theme->background, theme->light ? 0.92f : 0.86f, chrome) ||
        !color_with_alpha(theme->surface, surface_alpha, panel) ||
        !color_with_alpha(theme->surface, editor_alpha, editor) ||
        !color_with_alpha(theme->elevated, theme->light ? 0.88f : 0.78f, raised) ||
        !color_with_alpha(theme->primary, 0.15f, hover) ||
        !color_with_alpha(theme->primary, 0.32f, selected))
        return false;

    ThemeCssBuilder css = { .data = out, .capacity = capacity, .valid = true };
    css_append(&css,
        "*{scrollbar-track-color:%s;scrollbar-thumb-color:%s;scrollbar-thumb-active-color:%s;}"
        ".ca-titlebar{background:%s;}"
        ".ca-titlebar-title,.ca-titlebar-menu-item,.ca-titlebar-control{color:%s;}"
        ".ca-titlebar-menu-item:hover,.ca-titlebar-control:hover{background:%s;color:%s;}"
        ".ca-titlebar-close{color:%s;}.ca-titlebar-close:hover{background:%s;}"
        "splitter{background:transparent;color:%s;}"
        ".status-bar,.term-panel,.term-filler,.term-viewport{background:%s;}"
        ".status-bar-text{color:%s;}"
        ".status-bar-badge-key{background:%s;}.status-bar-badge-command{background:%s;}"
        ".status-bar-badge-leader{background:%s;}",
        chrome, selected, theme->primary, chrome, theme->secondary, hover,
        theme->text, theme->danger, hover, theme->primary, chrome, theme->muted,
        selected, hover, raised);

    css_append(&css,
        ".tree-panel,.plugin-side-panel{background:%s;}"
        ".tree-section-header{background:transparent;}"
        ".tree-section-title,.tree-arrow{color:%s;}"
        ".tree-row:hover,.tree-sticky-row:hover{background:%s;}"
        ".tree-sticky-row{background:%s;}"
        ".tree-arrow-open{color:%s;}.tree-name{color:%s;}.tree-name-dir{color:%s;}"
        ".buffer-tabs-row{background:%s;}"
        ".buffer-tab{background:transparent;}.buffer-tab:hover,.buffer-tab-close:hover{background:%s;}"
        ".buffer-tab-active{background:%s;}"
        ".buffer-tab-text{color:%s;}.buffer-tab-text-active{color:%s;}"
        ".buffer-pane,.buffer-pane-active,.buffer-body{background:%s;}"
        ".buffer-gutter-col,.buffer-scrollbar,.buffer-hscrollbar{background:%s;}"
        ".buffer-gutter-line{color:%s;}"
        ".buffer-line,.buffer-body-text,.hl-plain{color:%s;}"
        ".buffer-selection{background:%s;}.buffer-caret{background:%s;}"
        ".buffer-scrollbar-thumb,.buffer-scrollbar-thumb-active,.buffer-hscrollbar-thumb,.buffer-hscrollbar-thumb-active{background:%s;}"
        ".buffer-scrollbar-thumb:hover,.buffer-hscrollbar-thumb:hover{background:%s;}",
        panel, theme->muted, hover, raised, theme->primary, theme->secondary,
        theme->text, panel, hover, selected, theme->muted, theme->text, editor,
        chrome, theme->muted, theme->text, selected, theme->primary, selected,
        theme->primary);

    css_append(&css,
        ".hl-keyword,.hl-macro{color:%s;}.hl-comment{color:%s;}"
        ".hl-string,.hl-regex{color:%s;}"
        ".hl-number,.hl-constant,.hl-escape{color:%s;}"
        ".hl-function,.hl-constructor{color:%s;}"
        ".hl-type,.hl-namespace{color:%s;}"
        ".hl-property,.hl-attribute,.hl-parameter{color:%s;}"
        ".hl-operator,.hl-label{color:%s;}.hl-tag{color:%s;}"
        ".hl-bracket-1{color:%s;}.hl-bracket-2{color:%s;}.hl-bracket-3{color:%s;}"
        ".hl-bracket-4{color:%s;}.hl-bracket-5{color:%s;}.hl-bracket-6{color:%s;}",
        theme->primary, theme->muted, theme->success, theme->warning,
        theme->accent, theme->warning, theme->secondary, theme->accent,
        theme->danger, theme->warning, theme->danger, theme->accent,
        theme->success, theme->primary, theme->warning);

    css_append(&css,
        ".welcome-pane,.fp-list,.search-results,.pm-right,.sw-right,.scm-root,.scm-view{background:%s;}"
        ".welcome-title{color:%s;}"
        ".welcome-subtitle,.welcome-section-label,.term-tab{color:%s;}"
        ".welcome-desc{color:%s;}.welcome-hr,.sw-hr,.pm-hr{background:%s;}"
        ".welcome-btn,.pm-btn,.pm-btn-enable,.pm-btn-disable,.cf-row-key{background:%s;color:%s;}"
        ".welcome-btn-primary{background:%s;color:%s;}"
        ".welcome-btn:hover,.welcome-btn-primary:hover,.pm-btn:hover{background:%s;}"
        ".fp-root,.search-root-window,.pm-root,.sw-root,.cf-panel{background:%s;}"
        ".fp-toolbar,.fp-footer,.fp-colhdr,.search-header,.search-footer,.pm-left,.sw-left,.pm-search-row{background:%s;}"
        ".fp-row:hover,.search-result:hover,.pm-item:hover,.sw-tab-btn:hover{background:%s;}"
        ".fp-row-selected,.search-result-selected,.pm-item-selected,.sw-tab-btn-active{background:%s;}"
        ".fp-new-folder-input,.search-input,.pm-search-input,.sw-scale-input,.sw-select{background:%s;color:%s;}"
        ".search-result-line,.cf-row-key-text{color:%s;}"
        ".ca-popup-root{background:%s;}"
        ".ca-popup-card,.ca-select-popup,.ca-tooltip,.ca-context-menu,.ca-menubar-popup{background:%s;color:%s;}"
        ".ca-overlay-hover{background:%s;}.ca-overlay-selected{background:%s;color:%s;}",
        editor, theme->text, theme->muted, theme->secondary, selected, hover,
        theme->secondary, selected, theme->text, hover, raised, panel, hover,
        selected, editor, theme->text, theme->accent, chrome, raised,
        theme->text, hover, selected, theme->text);

    css_append(&css,
        ".scm-toolbar,.scm-repository,.scm-commit-box,.scm-section-header,.term-header{background:%s;}"
        ".scm-file-row:hover,.scm-commit-row:hover,.scm-branch-row:hover{background:%s;}"
        ".scm-commit-input,.scm-branch-input{background:%s;color:%s;}"
        ".scm-branch-row-current,.term-tab-active{background:%s;color:%s;}"
        ".term-cursor-focused{background:%s;}"
        ".tree-icon-dir-closed,.tree-icon-dir-open{color:%s;}"
        ".tree-icon-c,.tree-icon-ts{color:%s;}.tree-icon-h{color:%s;}"
        ".tree-icon-py{color:%s;}.tree-icon-js{color:%s;}"
        ".tree-icon-json,.tree-icon-html{color:%s;}.tree-icon-css{color:%s;}",
        panel, hover, editor, theme->text, selected, theme->text, theme->primary,
        theme->primary, theme->accent, theme->primary, theme->success,
        theme->warning, theme->danger, theme->accent);
    return css.valid && css.length > 0u;
}

/* Register every curated palette as a complete Glass-derived theme. */
static bool themes_on_load(SolPluginCtx *ctx)
{
    char css[THEME_CSS_CAPACITY];
    for (size_t i = 0u; i < sizeof(k_themes) / sizeof(k_themes[0]); ++i) {
        const ThemePalette *theme = &k_themes[i];
        if (!build_theme_css(theme, css, sizeof(css)) ||
            !sol_plugin_register_theme(ctx, &(SolThemeDesc){
                .id = theme->id,
                .name = theme->name,
                .base_id = "com.sol.theme.glass",
                .css = css,
                .colors = {
                    .background_rgb = packed_color(theme->background),
                    .primary_rgb = theme->primary_rgb,
                    .accent_rgb = theme->accent_rgb,
                },
                .has_colors = true,
            })) {
            sol_plugin_log(ctx, "failed to register theme '%s'", theme->id);
        }
    }
    return true;
}

/* Unregister every theme owned by this plugin. */
static void themes_on_unload(SolPluginCtx *ctx)
{
    for (size_t i = 0u; i < sizeof(k_themes) / sizeof(k_themes[0]); ++i)
        sol_plugin_unregister_theme(ctx, k_themes[i].id);
}

/* Publish the theme plugin descriptor. */
bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION || !out_api) return false;
    *out_api = (SolPluginAPI){
        .api_version = SOL_PLUGIN_API_VERSION,
        .id = "com.sol.themes",
        .display_name = "Sol Themes",
        .version = "2.0.0",
        .after = { NULL },
        .on_load = themes_on_load,
        .on_unload = themes_on_unload,
    };
    return true;
}
