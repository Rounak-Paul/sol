// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* Built-in widget-style descriptors and theme-aware CSS resolution. */

#ifndef SOL_UI_STYLE_REGISTRY_H
#define SOL_UI_STYLE_REGISTRY_H

#include "sol_theme.h"
#include "style.h"
#include "style_retro.h"

#include <stdlib.h>
#include <string.h>

typedef char *(*SolUIStyleBuildCssFn)(const SolThemeColors *colors);

typedef struct SolUIStyleDesc {
    const char *id;
    const char *name;
    const char *static_css;
    SolUIStyleBuildCssFn build_css;
} SolUIStyleDesc;

static const SolUIStyleDesc k_sol_ui_styles[] = {
    {
        .id = SOL_UI_STYLE_CLASSIC_ID,
        .name = SOL_UI_STYLE_CLASSIC_NAME,
        .static_css = SOL_UI_STYLE_CLASSIC_CSS,
    },
    {
        .id = SOL_UI_STYLE_RETRO_ID,
        .name = SOL_UI_STYLE_RETRO_NAME,
        .static_css = SOL_UI_STYLE_RETRO_CSS_PLACEHOLDER,
        .build_css = sol_retro_build_css,
    },
};

/* Register every built-in style in picker order. */
static inline bool sol_ui_register_builtin_styles(SolThemeRegistry *registry)
{
    if (!registry) return false;
    for (size_t i = 0u; i < sizeof(k_sol_ui_styles) / sizeof(k_sol_ui_styles[0]); ++i) {
        const SolUIStyleDesc *style = &k_sol_ui_styles[i];
        if (!sol_theme_register(registry, &(SolThemeDesc){
                .id = style->id,
                .name = style->name,
                .css = style->static_css,
            })) return false;
    }
    return true;
}

/*
 * Resolve a style against the active theme colors.
 *
 * id      Registered style identifier.
 * colors  Active theme palette anchors.
 * Returns heap CSS owned by the caller, or NULL when the style is unknown or allocation fails.
 */
static inline char *sol_ui_build_style_css(const char *id,
                                           const SolThemeColors *colors)
{
    if (!id) return NULL;
    for (size_t i = 0u; i < sizeof(k_sol_ui_styles) / sizeof(k_sol_ui_styles[0]); ++i) {
        const SolUIStyleDesc *style = &k_sol_ui_styles[i];
        if (strcmp(style->id, id) != 0) continue;
        if (style->build_css) return style->build_css(colors);
        const size_t length = strlen(style->static_css);
        char *css = (char *)malloc(length + 1u);
        if (!css) return NULL;
        memcpy(css, style->static_css, length + 1u);
        return css;
    }
    return NULL;
}

#endif /* SOL_UI_STYLE_REGISTRY_H */
