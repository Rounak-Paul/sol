// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_style.c — widget style overlay validation.
 *
 * The style overlays are large static CSS strings assembled from macros.
 * A syntax error in one of them is invisible at runtime: ca_css_parse
 * returns NULL, sol_ui_rebuild_stylesheet bails out, and the app silently
 * keeps the previously applied stylesheet — so the style simply "doesn't
 * work" with no diagnostic. These tests parse each overlay on its own and
 * in the same layered composition the UI system builds, so a malformed
 * rule fails the build instead of shipping.
 */

#include <causality.h>

#include "sol_settings.h"
#include "style.h"
#include "style_retro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "style test failed at %d: %s\n", __LINE__, #value); \
    return 1; \
} } while (0)

/* Parse css and free the result, reporting whether it was accepted. */
static bool style_parses(const char *css)
{
    Ca_Stylesheet *ss = ca_css_parse(css);
    if (!ss) return false;
    ca_css_destroy(ss);
    return true;
}

/* Concatenate theme + appearance + style exactly as the UI system does,
   then parse the result. Mirrors sol_ui_rebuild_stylesheet's layering. */
static bool style_composition_parses(const char *theme_css,
                                     const char *style_css)
{
    SolSettings settings = sol_settings_defaults();
    char overlay[8192];
    const int olen = sol_settings_build_appearance_css(&settings, overlay,
                                                       (int)sizeof(overlay));
    if (olen <= 0) return false;

    const size_t tlen = strlen(theme_css);
    const size_t slen = strlen(style_css);
    char *composed = (char *)malloc(tlen + (size_t)olen + slen + 1u);
    if (!composed) return false;
    char *dst = composed;
    memcpy(dst, theme_css, tlen);      dst += tlen;
    memcpy(dst, overlay, (size_t)olen); dst += olen;
    memcpy(dst, style_css, slen);      dst += slen;
    *dst = '\0';

    const bool ok = style_parses(composed);
    free(composed);
    return ok;
}

int main(void)
{
    /* The theme layer must parse standalone. */
    CHECK(style_parses(SOL_UI_DEFAULT_THEME_CSS));
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS, ".welcome-recent-session") != NULL);

    /* The workspace reserves its lower gutter through the status bar's
       margin, so only its top and sides contribute panel padding. */
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS,
                 ".workspace-main-content { padding: 8px 8px 0px; }") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS,
                 ".tree-section-header, .scm-header, .buffer-tabs-row, .term-header") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS,
                 ".scm-root, .scm-view { background: transparent; }") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS,
                 ".buffer-body, .term-panel {") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS,
                 ".term-viewport, .term-filler { background: transparent; }") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS, ".workspace-panel-chrome {") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS, ".workspace-panel-well {") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS, ".term-tab-new {") != NULL);
    CHECK(strstr(SOL_UI_DEFAULT_THEME_CSS, ".term-tab-new-icon {") != NULL);

    /* "Classic" adds no rules, but its body is a comment rather than ""
       because sol_theme_register rejects an empty CSS body — and a style
       that fails to register aborts UI-system creation entirely. */
    CHECK(style_parses(SOL_UI_STYLE_CLASSIC_CSS));
    CHECK(style_parses(SOL_UI_STYLE_RETRO_CSS_PLACEHOLDER));
    {
        const SolThemeColors colors = {
            .background_rgb = 0x000000u,
            .primary_rgb = 0x00e5ffu,
            .accent_rgb = 0x00e5ffu,
        };
        char *retro = sol_retro_build_css(&colors);
        CHECK(retro != NULL);
        CHECK(style_parses(retro));
        CHECK(style_composition_parses(SOL_UI_DEFAULT_THEME_CSS, retro));
        CHECK(strstr(retro, "border-top-color") != NULL);
        CHECK(strstr(retro, "border-bottom-color") != NULL);
        CHECK(strstr(retro, "border-radius:0px") != NULL);
        CHECK(strstr(retro, "background:") == NULL);
        CHECK(strstr(retro, "scrollbar-track-color") == NULL);
        CHECK(strstr(retro, "scrollbar-thumb-color") == NULL);
        CHECK(strstr(retro, "shadow-color") == NULL);
        CHECK(strstr(retro, "rgba(") == NULL);
        CHECK(strstr(retro, ".scm-submodule-card-conflict:hover") != NULL);
        const uint32_t highlight = sol_retro_mix(colors.background_rgb,
                                                  colors.primary_rgb, 112u);
        const uint32_t shadow = sol_retro_mix(highlight, 0x000000u, 176u);
        char expected_shadow[40];
        snprintf(expected_shadow, sizeof(expected_shadow),
                 "border-bottom-color:#%06x", shadow);
        CHECK(strstr(retro, expected_shadow) != NULL);
        free(retro);
    }

    /* Scrollbar uniformity: every native-scroll view (explorer, picker,
       plugin list, search, SCM sidebar + diff view) must follow the
       scrollbar_width setting through an explicit class rule — not just
       the `*` wildcard — so no theme or plugin `*` rule can silently
       diverge them from the buffer's custom scrollbars. Use a
       distinctive width so the assertion can't pass on defaults. */
    {
        SolSettings s = sol_settings_defaults();
        s.scrollbar_width = 13.5f;
        char overlay[8192];
        CHECK(sol_settings_build_appearance_css(&s, overlay,
                                                (int)sizeof(overlay)) > 0);
        CHECK(strstr(overlay, "scrollbar-width: 13.5px") != NULL);
        CHECK(strstr(overlay, ".native-scrollbar") != NULL);
        CHECK(strstr(overlay, ".term-tab-new,") != NULL);
        CHECK(strstr(overlay, ".workspace-panel-well, .buffer-body, .buffer-scroll-row { border-bottom-left-radius") != NULL);
        CHECK(strstr(overlay, ".buffer-body, .buffer-scroll-row { overflow: hidden; }") != NULL);
        CHECK(style_parses(overlay));
    }

    /* Retro must restyle the native overlay scrollbars in the same
       bevel language as the buffer trough/thumb — otherwise the
       explorer and diff view keep the glass look while buffers go
       bevelled. Width stays owned by the appearance overlay. */
    {
        const SolThemeColors colors = {
            .background_rgb = 0x1e1e26u,
            .primary_rgb = 0x60a5fau,
            .accent_rgb = 0xa78bfau,
        };
        char *retro = sol_retro_build_css(&colors);
        CHECK(retro != NULL);
        CHECK(strstr(retro, "scrollbar-track-color") == NULL);
        CHECK(strstr(retro, "scrollbar-thumb-color") == NULL);
        CHECK(strstr(retro, "scrollbar-thumb-active-color") == NULL);
        CHECK(strstr(retro, "scrollbar-radius") != NULL);
        CHECK(strstr(retro, "scrollbar-track-border-width") != NULL);
        CHECK(strstr(retro, "scrollbar-track-border-top-color") != NULL);
        CHECK(strstr(retro, "scrollbar-thumb-border-width") != NULL);
        CHECK(strstr(retro, "scrollbar-thumb-border-bottom-color") != NULL);
        CHECK(strstr(retro, "scrollbar-width") == NULL);
        CHECK(strstr(retro, ".native-scrollbar") != NULL);
        CHECK(strstr(retro, ".scm-root") != NULL);
        CHECK(strstr(retro, ".scm-header") != NULL);
        CHECK(strstr(retro, ".term-viewport") != NULL);
        CHECK(strstr(retro, ".term-tab-new") != NULL);
        CHECK(strstr(retro, ".workspace-panel") != NULL);
        CHECK(style_composition_parses(SOL_UI_DEFAULT_THEME_CSS, retro));
        free(retro);
    }

    puts("style overlay parsing and theme-aware bevel visibility passed");
    return 0;
}
