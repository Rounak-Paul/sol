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
    { "com.sol.theme.cyberpunk", "Cyberpunk", "#0d0015", "#160025", "#1e003a", "#f0e8ff", "#d0b8ff", "#8060a0", "#f0e040", "#ff2d78", "#ff2d78", "#00ff9f", "#f0e040", 0xf0e040, 0xff2d78, false },
    { "com.sol.theme.neon", "Neon", "#060010", "#0c0020", "#140030", "#f0e8ff", "#cc99ff", "#7040a0", "#ff00ff", "#00ffff", "#ff3366", "#00ff99", "#ffee00", 0xff00ff, 0x00ffff, false },
    { "com.sol.theme.synthwave", "Synthwave", "#1a0533", "#2a0845", "#360a58", "#f4e4ff", "#ddb8ff", "#9060c0", "#f92aad", "#36f9f6", "#ff355e", "#72f1b8", "#fede5d", 0xf92aad, 0x36f9f6, false },
    { "com.sol.theme.retro", "Retro", "#1a1200", "#2a1e00", "#362600", "#fff8e0", "#f0d890", "#a08020", "#e8a000", "#e84000", "#e84000", "#80c040", "#e8a000", 0xe8a000, 0xe84000, false },
    { "com.sol.theme.amber", "Amber", "#fefce8", "#fef3c7", "#fefce8", "#451a03", "#78350f", "#b45309", "#f59e0b", "#d97706", "#dc2626", "#16a34a", "#d97706", 0xf59e0b, 0xd97706, true },
    { "com.sol.theme.mint", "Mint", "#f0fdf9", "#d1fae5", "#f0fdf9", "#064e3b", "#065f46", "#059669", "#10b981", "#06b6d4", "#dc2626", "#10b981", "#d97706", 0x10b981, 0x06b6d4, true },
    { "com.sol.theme.lavender", "Lavender", "#faf5ff", "#ede9fe", "#faf5ff", "#2e1065", "#4c1d95", "#7c3aed", "#8b5cf6", "#ec4899", "#dc2626", "#16a34a", "#d97706", 0x8b5cf6, 0xec4899, true },
    { "com.sol.theme.peach", "Peach", "#fff7ed", "#ffedd5", "#fff7ed", "#431407", "#7c2d12", "#c2410c", "#f97316", "#ec4899", "#dc2626", "#16a34a", "#d97706", 0xf97316, 0xec4899, true },
    { "com.sol.theme.sky", "Sky", "#f0f9ff", "#e0f2fe", "#f0f9ff", "#082f49", "#0c4a6e", "#0369a1", "#0284c7", "#7c3aed", "#dc2626", "#16a34a", "#d97706", 0x0284c7, 0x7c3aed, true },
    { "com.sol.theme.lemon", "Lemon", "#fefce8", "#fef9c3", "#fefce8", "#422006", "#713f12", "#a16207", "#ca8a04", "#16a34a", "#dc2626", "#16a34a", "#ca8a04", 0xca8a04, 0x16a34a, true },
    { "com.sol.theme.moonlight", "Moonlight", "#1f2335", "#24283b", "#2a2f45", "#dcd7ba", "#c8c093", "#727169", "#7e9cd8", "#957fb8", "#e46876", "#98bb6c", "#e6c384", 0x7e9cd8, 0x957fb8, false },
    { "com.sol.theme.kanagawa", "Kanagawa", "#1f1f28", "#2a2a37", "#363646", "#dcd7ba", "#c8c093", "#727169", "#7e9cd8", "#957fb8", "#e46876", "#98bb6c", "#e6c384", 0x7e9cd8, 0x957fb8, false },
    { "com.sol.theme.everforest", "Everforest", "#2d353b", "#343f44", "#3d484d", "#d3c6aa", "#c4b49a", "#9da9a0", "#a7c080", "#83c092", "#e67e80", "#a7c080", "#dbbc7f", 0xa7c080, 0x83c092, false },
    { "com.sol.theme.rose-pine", "Rosé Pine", "#191724", "#1f1d2e", "#26233a", "#e0def4", "#c4c2da", "#6e6a86", "#ebbcba", "#c4a7e7", "#eb6f92", "#9ccfd8", "#f6c177", 0xebbcba, 0xc4a7e7, false },
    { "com.sol.theme.ayu-dark", "Ayu Dark", "#0d1017", "#131721", "#1a2130", "#bfbdb6", "#a8a6a0", "#626672", "#ffb454", "#73d0ff", "#f28779", "#aad94c", "#ffb454", 0xffb454, 0x73d0ff, false },
    { "com.sol.theme.ayu-light", "Ayu Light", "#fafafa", "#f3f4f5", "#fafafa", "#575f66", "#4d5560", "#8a9199", "#f2ae49", "#399ee6", "#e65050", "#86b300", "#f2ae49", 0xf2ae49, 0x399ee6, true },
    { "com.sol.theme.one-dark", "One Dark", "#282c34", "#21252b", "#2c313a", "#abb2bf", "#9da5b4", "#5c6370", "#61afef", "#c678dd", "#e06c75", "#98c379", "#e5c07b", 0x61afef, 0xc678dd, false },
    { "com.sol.theme.one-light", "One Light", "#fafafa", "#f2f2f2", "#fafafa", "#383a42", "#50545c", "#696c77", "#4078f2", "#a626a4", "#e45649", "#50a14f", "#c18401", 0x4078f2, 0xa626a4, true },
    { "com.sol.theme.material-dark", "Material Dark", "#212121", "#2d2d2d", "#383838", "#eeffff", "#d0d0d0", "#546e7a", "#82aaff", "#c3e88d", "#f07178", "#c3e88d", "#ffcb6b", 0x82aaff, 0xc3e88d, false },
    { "com.sol.theme.material-light", "Material Light", "#fafafa", "#ffffff", "#fafafa", "#212121", "#424242", "#757575", "#6200ee", "#03dac6", "#b00020", "#00875a", "#ff6d00", 0x6200ee, 0x03dac6, true },
    { "com.sol.theme.palenight", "Palenight", "#292d3e", "#1b1e2b", "#232635", "#a6accd", "#959dcb", "#676e95", "#82aaff", "#c792ea", "#f07178", "#c3e88d", "#ffcb6b", 0x82aaff, 0xc792ea, false },
    { "com.sol.theme.panda", "Panda", "#1a1b26", "#1e2030", "#252840", "#e6e6e6", "#d4d4d4", "#6c6f93", "#ff75b5", "#19f9d8", "#ff2c6d", "#19f9d8", "#ffb86c", 0xff75b5, 0x19f9d8, false },
    { "com.sol.theme.horizon", "Horizon", "#1c1e26", "#232530", "#2e303e", "#d5d8da", "#bec0cc", "#6c6f8f", "#e95678", "#fab795", "#e95678", "#09f7a0", "#fab795", 0xe95678, 0xfab795, false },
    { "com.sol.theme.pitch-black", "Pitch Black", "#000000", "#060606", "#0c0c0c", "#cccccc", "#999999", "#555555", "#888888", "#666666", "#cc4444", "#44aa44", "#aa7700", 0x888888, 0x666666, false },
    { "com.sol.theme.paper", "Paper", "#f7f4ef", "#fffef8", "#f7f4ef", "#1a1a1a", "#333333", "#777777", "#555555", "#888888", "#cc3333", "#336633", "#996600", 0x555555, 0x888888, true },
    { "com.sol.theme.newspaper", "Newspaper", "#f5f0e8", "#faf7f2", "#f5f0e8", "#111111", "#2d2d2d", "#666666", "#1a1a1a", "#cc0000", "#cc0000", "#336633", "#996600", 0x1a1a1a, 0xcc0000, true },
    { "com.sol.theme.ink", "Ink", "#111418", "#181c20", "#1e2228", "#e0e4e8", "#c0c8d0", "#6080a0", "#4488cc", "#88ccaa", "#cc6644", "#66aa88", "#ccaa44", 0x4488cc, 0x88ccaa, false },
    { "com.sol.theme.dusk", "Dusk", "#1e1028", "#261638", "#301e48", "#f0e8ff", "#d8c8f0", "#8868a8", "#c084fc", "#fb7185", "#fb7185", "#34d399", "#fbbf24", 0xc084fc, 0xfb7185, false },
    { "com.sol.theme.pastel", "Pastel", "#fdf8ff", "#fef3fb", "#fdf8ff", "#3d2c5e", "#5a4278", "#9880b8", "#a78bfa", "#f9a8d4", "#e879a0", "#34d399", "#f59e0b", 0xa78bfa, 0xf9a8d4, true },
    { "com.sol.theme.teal", "Teal", "#f0fdfa", "#ccfbf1", "#f0fdfa", "#042f2e", "#134e4a", "#0f766e", "#0d9488", "#0891b2", "#dc2626", "#0d9488", "#d97706", 0x0d9488, 0x0891b2, true },
    { "com.sol.theme.woodland", "Woodland", "#f5f0e8", "#ece4d4", "#f5f0e8", "#2a2018", "#3e3020", "#6e6040", "#7a8c5a", "#c8a96e", "#c0392b", "#7a8c5a", "#c8a96e", 0x7a8c5a, 0xc8a96e, true },
    { "com.sol.theme.desert", "Desert", "#f5ede0", "#ecdbc6", "#f5ede0", "#2e1a08", "#4a2e10", "#8a6030", "#d4955a", "#c8a050", "#c0392b", "#5a8a40", "#c8a050", 0xd4955a, 0xc8a050, true },
    { "com.sol.theme.volcano", "Volcano", "#1a0800", "#220a00", "#2e1000", "#fff4e8", "#ffd0a8", "#c06030", "#ff4422", "#ffaa00", "#ff4422", "#44cc44", "#ffaa00", 0xff4422, 0xffaa00, false },
    { "com.sol.theme.deep-sea", "Deep Sea", "#020e18", "#051824", "#092030", "#b2ebf2", "#80deea", "#006064", "#00bcd4", "#00e676", "#ff5252", "#00e676", "#ffc107", 0x00bcd4, 0x00e676, false },
    { "com.sol.theme.grape", "Grape", "#16001e", "#1e0030", "#280040", "#f3e5f5", "#e1bee7", "#7b1fa2", "#9c27b0", "#e040fb", "#f44336", "#66bb6a", "#ffa726", 0x9c27b0, 0xe040fb, false },
    { "com.sol.theme.ash", "Ash", "#263238", "#2e3c43", "#37474f", "#eceff1", "#cfd8dc", "#78909c", "#78909c", "#4db6ac", "#ef5350", "#66bb6a", "#ffa726", 0x78909c, 0x4db6ac, false },
    { "com.sol.theme.crimson", "Crimson", "#12000a", "#1e000e", "#280014", "#fff0f3", "#ffccd5", "#aa3050", "#dc143c", "#ff6b6b", "#dc143c", "#44bb44", "#ffaa00", 0xdc143c, 0xff6b6b, false },
    { "com.sol.theme.ice", "Ice", "#f0f8ff", "#e0f0ff", "#f0f8ff", "#0a2540", "#103a60", "#3a7aaa", "#7dd3fc", "#a5f3fc", "#dc2626", "#059669", "#d97706", 0x7dd3fc, 0xa5f3fc, true },
    { "com.sol.theme.coral", "Coral", "#fff5f5", "#ffe4e4", "#fff5f5", "#4a0808", "#7a1818", "#c05050", "#ff6b6b", "#ffd166", "#dc2626", "#16a34a", "#d97706", 0xff6b6b, 0xffd166, true },
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
    char chrome[32], panel[32], editor[32], raised[32], popup_bg[32], hover[32], selected[32];
    const float surface_alpha = theme->light ? 0.62f : 0.48f;
    const float editor_alpha = theme->light ? 0.58f : 0.45f;
    if (!color_with_alpha(theme->background, theme->light ? 0.78f : 0.68f, chrome) ||
        !color_with_alpha(theme->surface, surface_alpha, panel) ||
        !color_with_alpha(theme->surface, editor_alpha, editor) ||
        !color_with_alpha(theme->elevated, theme->light ? 0.88f : 0.78f, raised) ||
        !color_with_alpha(theme->elevated, 1.0f, popup_bg) ||
        !color_with_alpha(theme->primary, 0.15f, hover) ||
        !color_with_alpha(theme->primary, 0.32f, selected))
        return false;

    ThemeCssBuilder css = { .data = out, .capacity = capacity, .valid = true };
    css_append(&css,
        "*{scrollbar-track-color:transparent;scrollbar-thumb-color:%s;scrollbar-thumb-active-color:%s;}"
        ".ca-titlebar{background:%s;}"
        ".ca-titlebar-title,.ca-titlebar-menu-item,.ca-titlebar-control{color:%s;}"
        ".ca-titlebar-menu-item:hover,.ca-titlebar-control:hover{background:%s;color:%s;}"
        ".ca-titlebar-close{color:%s;}.ca-titlebar-close:hover{background:%s;}"
        "splitter{background:transparent;color:%s;}"
        ".status-bar,.term-panel{background:%s;}"
        ".term-filler,.term-viewport{background:transparent;}"
        ".status-bar-text{color:%s;}"
        ".status-bar-badge-key{background:%s;}.status-bar-badge-command{background:%s;}"
        ".status-bar-badge-leader{background:%s;}",
        selected, theme->primary, chrome, theme->secondary, hover,
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
        ".buffer-pane,.buffer-pane-active{background:%s;}.buffer-body{background:transparent;}"
        ".buffer-gutter-col,.buffer-scrollbar,.buffer-hscrollbar{background:transparent;}"
        ".buffer-gutter-line{color:%s;}"
        ".buffer-line,.buffer-body-text,.hl-plain{color:%s;}"
        ".buffer-selection{background:%s;}.buffer-caret{background:%s;}"
        ".buffer-scrollbar-thumb,.buffer-scrollbar-thumb-active,.buffer-hscrollbar-thumb,.buffer-hscrollbar-thumb-active{background:%s;}"
        ".buffer-scrollbar-thumb:hover,.buffer-hscrollbar-thumb:hover{background:%s;}",
        panel, theme->muted, hover, raised, theme->primary, theme->secondary,
        theme->text, panel, hover, selected, theme->muted, theme->text, editor,
        theme->muted, theme->text, selected, theme->primary, selected,
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
        selected, editor, theme->text, theme->accent, chrome, popup_bg,
        theme->text, hover, selected, theme->text);

    css_append(&css,
        ".scm-toolbar,.scm-repository,.scm-commit-box,.scm-section-header,.term-header{background:%s;}"
        ".scm-file-row:hover,.scm-commit-row:hover,.scm-branch-row:hover,.scm-submodule-row:hover{background:%s;}"
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

    /* Source-control panel accent/status roles. These classes were added
       after the block above was written, so without this they stayed on
       their compile-time fallback colors in style.h regardless of the
       active theme — the panel visually disagreed with everything else
       once a non-default theme was selected. Primary/accent drive the
       panel's interactive chrome (title icon, branch icon, active tab,
       CTA buttons); success/danger/warning/accent drive git status
       semantics (added/modified/deleted/conflict, ahead/behind, busy). */
    css_append(&css,
        ".scm-title-icon,.scm-branch-icon,.scm-clean-icon{color:%s;}"
        ".scm-icon-action:hover,.scm-header-icon-action:hover,"
        ".scm-action-icon:hover{background:%s;color:%s;}"
        ".scm-primary-action{background:%s;color:%s;}"
        ".scm-primary-action:hover{background:%s;}"
        ".scm-danger-action,.scm-icon-danger{color:%s;}"
        ".scm-danger-action:hover,.scm-icon-danger:hover{background:%s;color:%s;}"
        ".scm-tab:hover{background:%s;color:%s;}"
        ".scm-tab-active{color:%s;}"
        ".scm-repository-status-busy,.scm-repository-status-sync,.scm-activity-icon{color:%s;}"
        ".scm-repository-status-clean{color:%s;}"
        ".scm-repository-status-changed{color:%s;}"
        ".scm-repository-status-error{color:%s;}"
        ".scm-status-added,.scm-sync-ahead{color:%s;}"
        ".scm-status-modified,.scm-sync-behind{color:%s;}"
        ".scm-status-deleted,.scm-error-icon,.scm-error-text{color:%s;}"
        ".scm-status-renamed{color:%s;}.scm-status-conflict{color:%s;}"
        ".scm-branch-current-icon{color:%s;}"
        ".scm-graph-line-active{background:%s;}"
        ".scm-graph-dot-merge{background:%s;}",
        /* 1 */ theme->secondary,
        /* 2 */ hover, /* 3 */ theme->text,
        /* 4 */ theme->primary, /* 5 */ theme->text,
        /* 6 */ theme->accent,
        /* 7 */ theme->danger,
        /* 8 */ hover, /* 9 */ theme->danger,
        /* 10 */ hover, /* 11 */ theme->text,
        /* 12 */ theme->primary,
        /* 13 */ theme->accent,
        /* 14 */ theme->success,
        /* 15 */ theme->warning,
        /* 16 */ theme->danger,
        /* 17 */ theme->success,
        /* 18 */ theme->warning,
        /* 19 */ theme->danger,
        /* 20 */ theme->accent, /* 21 */ theme->danger,
        /* 22 */ theme->primary,
        /* 23 */ theme->primary,
        /* 24 */ theme->accent);
    css_append(&css,
        ".scm-graph-connector{background:%s;}"
        ".scm-submodule-clean{color:%s;}"
        ".scm-submodule-modified{color:%s;}"
        ".scm-submodule-warning{color:%s;}"
        ".scm-submodule-conflict{color:%s;}",
        theme->primary, theme->muted, theme->warning, theme->warning,
        theme->danger);
    css_append(&css,
        ".scm-remote-action{color:%s;}"
        ".scm-remote-action:hover{background:%s;color:%s;}"
        ".scm-remote-action-icon{color:%s;}"
        ".scm-remote-action-pull-ready{background:%s;}"
        ".scm-remote-action-push-ready{background:%s;}"
        ".scm-remote-action-icon-pull-ready{color:%s;}"
        ".scm-remote-action-icon-push-ready{color:%s;}",
        theme->muted, hover, theme->text, theme->primary, hover, hover,
        theme->warning, theme->success);
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
        .version = "2.1.0",
        .after = { NULL },
        .on_load = themes_on_load,
        .on_unload = themes_on_unload,
    };
    return true;
}
