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

/* Relative luminance (0..1) of a packed 0xRRGGBB colour. */
static double luminance(uint32_t rgb)
{
    const double r = (double)((rgb >> 16) & 0xffu);
    const double g = (double)((rgb >> 8) & 0xffu);
    const double b = (double)(rgb & 0xffu);
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}

/*
 * Assert both bevel edges are perceptually distinct from the widget
 * surface they sit on for the given theme background.
 *
 * A bevel only reads as 3D when the highlight is lighter and the shadow
 * darker than the surface by a visible margin. The original fixed-alpha
 * tones failed this at both ends of the range (invisible shadow on
 * near-black themes, invisible highlight on paper-white ones), which is
 * the regression this guards.
 */
static bool bevel_is_visible(uint32_t background)
{
    const uint32_t surface = sol_retro_widget_surface(background);
    uint32_t light = 0u, dark = 0u;
    sol_retro_bevel_tones(surface, &light, &dark);

    const double ls = luminance(surface);
    const double ll = luminance(light);
    const double ld = luminance(dark);

    /* Each edge must move at least this far in luminance to register as
       relief rather than as a hairline of almost the same colour. */
    const double min_delta = 0.04;
    return (ll - ls) >= min_delta && (ls - ld) >= min_delta;
}

int main(void)
{
    /* The theme layer must parse standalone. */
    CHECK(style_parses(SOL_UI_DEFAULT_THEME_CSS));

    /* "Classic" adds no rules, but its body is a comment rather than ""
       because sol_theme_register rejects an empty CSS body — and a style
       that fails to register aborts UI-system creation entirely. */
    CHECK(style_parses(SOL_UI_STYLE_CLASSIC_CSS));
    CHECK(style_parses(SOL_UI_STYLE_RETRO_CSS_PLACEHOLDER));

    /* Retro CSS is generated per theme background. Generate for a spread
       of backgrounds and require each to parse in the full composition. */
    static const uint32_t backgrounds[] = {
        0x000000u,  /* pure black — no footroom for a shadow   */
        0x06080fu,  /* Sol's own near-black glass background   */
        0x0d0d11u,  /* midnight                                */
        0x1e1e26u,  /* dark slate                              */
        0x808080u,  /* mid grey — the era's native surface     */
        0xf5f5f0u,  /* paper                                   */
        0xffffffu,  /* pure white — no headroom for a highlight */
    };

    for (size_t i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]); ++i) {
        char retro[SOL_RETRO_CSS_MAX];
        const int len = sol_retro_build_css(backgrounds[i], retro,
                                            (int)sizeof(retro));
        CHECK(len > 0);
        CHECK(style_parses(retro));
        CHECK(style_composition_parses(SOL_UI_DEFAULT_THEME_CSS, retro));

        /* Per-side colours and square corners are the whole premise: a
           future edit collapsing them into a uniform `border` would make
           Causality paint a flat outline, silently losing the 3D effect. */
        CHECK(strstr(retro, "border-top-color") != NULL);
        CHECK(strstr(retro, "border-left-color") != NULL);
        CHECK(strstr(retro, "border-bottom-color") != NULL);
        CHECK(strstr(retro, "border-right-color") != NULL);
        CHECK(strstr(retro, "border-radius: 0px") != NULL);

        /* Both edges visible on every background, including the extremes
           where one direction has no room and the other must compensate. */
        CHECK(bevel_is_visible(backgrounds[i]));
    }

    /* The surface lift is what makes the extremes work: a near-black
       theme background must be raised into the bevel-capable band, not
       used as the widget surface directly. */
    CHECK(sol_retro_widget_surface(0x000000u) != 0x000000u);
    CHECK(sol_retro_widget_surface(0xffffffu) != 0xffffffu);
    /* A surface already inside the band is left alone. */
    CHECK(sol_retro_widget_surface(0x808080u) == 0x808080u);

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
        CHECK(strstr(overlay, ".tree-scroll-area") != NULL);
        CHECK(strstr(overlay, ".fp-list") != NULL);
        CHECK(strstr(overlay, ".pm-list") != NULL);
        CHECK(strstr(overlay, ".search-results") != NULL);
        CHECK(strstr(overlay, ".search-preview-code") != NULL);
        CHECK(strstr(overlay, ".scm-content") != NULL);
        CHECK(strstr(overlay, ".scm-view") != NULL);
        CHECK(style_parses(overlay));
    }

    /* Retro must restyle the native overlay scrollbars in the same
       bevel language as the buffer trough/thumb — otherwise the
       explorer and diff view keep the glass look while buffers go
       bevelled. Width stays owned by the appearance overlay. */
    {
        char retro[SOL_RETRO_CSS_MAX];
        CHECK(sol_retro_build_css(0x1e1e26u, retro,
                                  (int)sizeof(retro)) > 0);
        CHECK(strstr(retro, "scrollbar-track-color") != NULL);
        CHECK(strstr(retro, "scrollbar-thumb-color") != NULL);
        CHECK(strstr(retro, "scrollbar-thumb-active-color") != NULL);
        CHECK(strstr(retro, "scrollbar-radius") != NULL);
        CHECK(strstr(retro, "scrollbar-width") == NULL);
        CHECK(strstr(retro, ".tree-scroll-area") != NULL);
        CHECK(strstr(retro, ".scm-view") != NULL);
        CHECK(style_composition_parses(SOL_UI_DEFAULT_THEME_CSS, retro));
    }

    puts("style overlay parsing and theme-aware bevel visibility passed");
    return 0;
}
