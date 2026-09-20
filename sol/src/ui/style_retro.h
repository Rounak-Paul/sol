// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* style_retro.h — "Retro" widget style overlay.
 *
 * A style is the relief/shape language of the UI: border reliefs, corner
 * radii and edge treatments. It is orthogonal to the colour theme (which
 * supplies hues) and to the background effect (which paints behind the
 * workspace), and is layered last so it wins over both the theme CSS and
 * the appearance-slider overlay (see sol_ui_rebuild_stylesheet).
 *
 * This overlay recreates the classic 3D chiselled look of mid-90s desktop
 * toolkits. Two primitives do all the work, both built from Causality's
 * per-side border colours (paint.c paints each side separately whenever
 * the four side colours are not identical, with correct diagonal corner
 * joints — so a two-tone border renders as a true bevel):
 *
 *   raised   light on top/left, dark on bottom/right — buttons, tabs,
 *            title bars, anything that should read as standing proud of
 *            the surface and clickable.
 *   sunken   dark on top/left, light on bottom/right — text areas, lists,
 *            scroll wells, inputs: containers that content sits *inside*.
 *
 * ---- Why the tones are generated, not fixed ------------------------------
 *
 * A bevel is two edges pushed in opposite luminance directions away from
 * the surface they sit on, so it can only read if the surface has room
 * both above and below it. Fixed translucent tones (white@55% over the
 * surface for the highlight, black@55% for the shadow) fail at both ends
 * of the range: on Sol's near-black themes the shadow lands within ~1.05
 * contrast of the background and disappears, leaving widgets looking like
 * they have a highlight on two sides instead of a relief; on a paper-white
 * theme the *highlight* disappears the same way. Only a mid-grey surface —
 * which is exactly what the era's desktops used — works with fixed tones.
 *
 * So the tones are derived per theme in two steps:
 *
 *   1. sol_retro_widget_surface lifts a too-dark surface (or drops a
 *      too-bright one) into the luminance band where a two-sided bevel is
 *      physically representable. This is why bevelled widgets read as
 *      slightly lighter panels on a dark theme rather than being flush
 *      with the desktop — the same reason real toolkits gave controls
 *      their own grey.
 *   2. sol_retro_bevel_tones offsets from that surface by a fixed step in
 *      each direction, clamped to what the channel range allows.
 *
 * Corner radii are forced to 0 throughout: a rounded bevel immediately
 * reads as a modern imitation rather than the real thing, and Causality
 * draws the per-side edges as straight rects anyway, so any radius would
 * leave the bevel corners visibly clipped against the rounded background.
 *
 * Backdrop blur is likewise zeroed. The retro look is flat opaque panels
 * with hard edges; a blurred translucent surface behind a chiselled
 * border reads as two different design languages fighting each other.
 */

#ifndef SOL_UI_STYLE_RETRO_H
#define SOL_UI_STYLE_RETRO_H

#include <stdint.h>
#include <stdio.h>

/* Bevel geometry. 2px is the authentic double-line width of the era's
   "window edge"; 1px is used for controls that are small enough that a
   2px bevel would eat their whole interior (tabs, list rows, scrollbars). */
#define SOL_RETRO_BEVEL_PX      "2px"
#define SOL_RETRO_BEVEL_THIN_PX "1px"

/* Luminance band, per 0-255 channel, in which a two-sided bevel fits.
   Below the floor there is no room for a shadow, above the ceiling none
   for a highlight, so surfaces outside the band are moved into it. */
#define SOL_RETRO_SURFACE_FLOOR 58
#define SOL_RETRO_SURFACE_CEIL  200

/* Per-edge luminance offset from the widget surface. Large enough to read
   at a 1px width on a text-dense UI without looking like a drawn outline. */
#define SOL_RETRO_EDGE_STEP 52

/* Sunken wells are cut *into* the widget surface, so they sit a step below
   it — a text area flush with its frame reads as flat. */
#define SOL_RETRO_WELL_DROP 26

/* Longest CSS this overlay can emit, including the generated colours. */
#define SOL_RETRO_CSS_MAX 12288u

/*
 * Move a surface colour into the band where a two-sided bevel is
 * representable, preserving hue by shifting every channel equally.
 *
 * rgb  Packed 0xRRGGBB source surface (typically the theme background).
 * Returns the packed adjusted surface.
 */
static inline uint32_t sol_retro_widget_surface(uint32_t rgb)
{
    int r = (int)((rgb >> 16) & 0xffu);
    int g = (int)((rgb >> 8) & 0xffu);
    int b = (int)(rgb & 0xffu);

    const int brightest = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int darkest   = r < g ? (r < b ? r : b) : (g < b ? g : b);

    int delta = 0;
    if (brightest < SOL_RETRO_SURFACE_FLOOR) {
        delta = SOL_RETRO_SURFACE_FLOOR - brightest;   /* lift */
    } else if (darkest > SOL_RETRO_SURFACE_CEIL) {
        delta = SOL_RETRO_SURFACE_CEIL - darkest;      /* drop (negative) */
    }

    r += delta; g += delta; b += delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Offset every channel of a packed colour by delta, clamped to 0..255. */
static inline uint32_t sol_retro_shift(uint32_t rgb, int delta)
{
    int r = (int)((rgb >> 16) & 0xffu) + delta;
    int g = (int)((rgb >> 8) & 0xffu) + delta;
    int b = (int)(rgb & 0xffu) + delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Derive the highlight and shadow edge tones for a widget surface.
 *
 * Each edge is offset by SOL_RETRO_EDGE_STEP, but clamped to the room the
 * channel range actually leaves. Whatever one edge cannot use is handed to
 * the other, so a surface pinned against either end of the range still
 * shows the same total relief through its one usable edge instead of
 * silently losing half the bevel.
 *
 * surface    Packed 0xRRGGBB widget surface.
 * out_light  Receives the packed highlight tone.
 * out_dark   Receives the packed shadow tone.
 */
static inline void sol_retro_bevel_tones(uint32_t surface,
                                         uint32_t *out_light,
                                         uint32_t *out_dark)
{
    const int r = (int)((surface >> 16) & 0xffu);
    const int g = (int)((surface >> 8) & 0xffu);
    const int b = (int)(surface & 0xffu);
    const int brightest = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int darkest   = r < g ? (r < b ? r : b) : (g < b ? g : b);

    const int head = 255 - brightest;   /* room for a highlight */
    const int foot = darkest;           /* room for a shadow    */

    int up = SOL_RETRO_EDGE_STEP < head ? SOL_RETRO_EDGE_STEP : head;
    int dn = SOL_RETRO_EDGE_STEP < foot ? SOL_RETRO_EDGE_STEP : foot;

    /* Redistribute the shortfall to whichever edge still has room. */
    if (up < SOL_RETRO_EDGE_STEP) {
        const int want = dn + (SOL_RETRO_EDGE_STEP - up);
        dn = want < foot ? want : foot;
    }
    if (dn < SOL_RETRO_EDGE_STEP) {
        const int want = up + (SOL_RETRO_EDGE_STEP - dn);
        up = want < head ? want : head;
    }

    *out_light = sol_retro_shift(surface, up);
    *out_dark  = sol_retro_shift(surface, -dn);
}

/*
 * Emit the Retro style CSS for a given theme background.
 *
 * background_rgb  Packed 0xRRGGBB theme background, used to derive the
 *                 widget surface and both bevel tones.
 * buf             Destination buffer.
 * bufsz           Bytes available in buf (SOL_RETRO_CSS_MAX is enough).
 * Returns the number of bytes written excluding the terminator, or 0 when
 * the arguments are invalid or the CSS did not fit.
 */
static inline int sol_retro_build_css(uint32_t background_rgb,
                                      char *buf, int bufsz)
{
    if (!buf || bufsz <= 0) return 0;

    const uint32_t surface = sol_retro_widget_surface(background_rgb);
    uint32_t light = 0u, dark = 0u;
    sol_retro_bevel_tones(surface, &light, &dark);

    /* Wells (text areas, lists, troughs) are cut into the surface, and
       their own bevel is derived from that lower tone so the groove edge
       stays visible against the well rather than against the frame. */
    const uint32_t well = sol_retro_shift(surface, -SOL_RETRO_WELL_DROP);
    uint32_t well_light = 0u, well_dark = 0u;
    sol_retro_bevel_tones(well, &well_light, &well_dark);

    /* A pressed control swaps relief AND darkens slightly: on a mid-tone
       surface the inverted bevel alone is subtle, and the era's toolkits
       dropped the face colour too. */
    const uint32_t pressed = sol_retro_shift(surface, -12);

    const int n = snprintf(buf, (size_t)bufsz,
        "/* ===== Retro style overlay (generated) ===== */"

        /* Kill every radius and blur first, so anything not explicitly
           bevelled below still lands in the right design language.
           Specificity 0, so every class rule below still wins. */
        "* {"
        "  border-radius: 0px;"
        "  backdrop-filter: blur(0px);"
        "}"

        /* ---- Native overlay scrollbars (explorer, diff view, search,
           picker, plugin list): share the buffer's surface tones. Width
           remains owned by the appearance overlay. */
        ".native-scrollbar {"
        "  scrollbar-track-color: #%06x;"
        "  scrollbar-thumb-color: #%06x;"
        "  scrollbar-thumb-active-color: #%06x;"
        "  scrollbar-radius: 0px;"
        "}"

        /* ---- Workspace panels: framed surfaces holding content ---- */
        ".workspace-panel, .tree-panel, .plugin-side-panel, .buffer-pane, .term-panel,"
        ".welcome-pane, .term-float-panel {"
        "  background: #%06x;"
        "  border-top-width: 2px; border-left-width: 2px;"
        "  border-bottom-width: 2px; border-right-width: 2px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        /* Focused panel keeps the accent ring as a flat inset line: a
           second bevel over the panel's own reads as a doubled edge
           rather than a focus cue. */
        ".tree-panel-focused, .plugin-side-panel-focused,"
        ".buffer-pane-focused, .term-panel-focused {"
        "  border-width: 1px;"
        "  border-color: rgba(255, 170, 40, 0.95);"
        "  border-radius: 0px;"
        "}"

        /* ---- Wells: content sits inside, so sunken and a step darker ---- */
        ".workspace-panel-well, .buffer-body, .buffer-scroll-row, .term-viewport, .term-filler,"
        ".tree-scroll-area, .scm-root,"
        ".pm-list, .fp-list, .search-results, .pm-right, .sw-right {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"

        /* ---- Tab strips: raised surface carrying the tabs ---- */
        ".workspace-panel-chrome, .buffer-tabs-row, .term-header, .project-tabs, .scm-header,"
        ".scm-tabs, .scm-section-header {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        /* Inactive tabs stay flush with the strip; only the active one
           pops. Raising every tab loses any sense of which is current. */
        ".buffer-tab, .term-tab, .project-tab, .scm-tab, .sw-tab-btn {"
        "  border-radius: 0px; border-width: 0px;"
        "}"
        ".buffer-tab-active, .term-tab-active, .project-tab-active,"
        ".scm-tab-active, .sw-tab-btn-active {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"

        /* ---- Buttons: raised face, inverting on press ---- */
        ".ca-popup-btn, .ca-titlebar-control, .ca-titlebar-menu-item,"
        ".pm-btn, .pm-btn-enable, .pm-btn-disable,"
        ".fp-up-btn, .fp-crumb-btn, .fp-colhdr-btn, .fp-colhdr-btn-end,"
        ".fp-action-cancel, .fp-action-primary, .fp-action-new-folder,"
        ".fp-nf-create, .fp-nf-cancel,"
        ".welcome-btn, .welcome-btn-primary,"
        ".scm-header-action, .scm-action, .scm-icon-action,"
        ".scm-header-icon-action, .scm-header-close-action, .scm-action-icon,"
        ".scm-branch-create-from, .scm-section-action, .scm-primary-action,"
        ".scm-danger-action, .scm-remote-action,"
        ".buffer-tab-close, .term-tab-close, .project-tab-close,"
        ".project-tab-new, .status-bar-badge, .cf-row-key, .pm-badge {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        /* Pressed state inverts the bevel — the most recognisable
           affordance of this era, and why the buttons feel physical. */
        ".ca-popup-btn:active, .ca-titlebar-control:active,"
        ".ca-titlebar-menu-item:active,"
        ".pm-btn:active, .pm-btn-enable:active, .pm-btn-disable:active,"
        ".fp-up-btn:active, .fp-crumb-btn:active,"
        ".fp-action-cancel:active, .fp-action-primary:active,"
        ".fp-action-new-folder:active, .fp-nf-create:active,"
        ".fp-nf-cancel:active,"
        ".welcome-btn:active, .welcome-btn-primary:active,"
        ".scm-action:active, .scm-primary-action:active,"
        ".scm-danger-action:active,"
        ".buffer-tab-close:active, .term-tab-close:active,"
        ".project-tab-close:active, .project-tab-new:active {"
        "  background: #%06x;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "}"

        /* ---- Inputs: always sunken wells, they receive content ---- */
        ".fp-new-folder-input, .search-input, .pm-search-input,"
        ".sw-scale-input, .sw-select, .scm-commit-input, .scm-branch-input {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"

        /* ---- Scrollbars: sunken trough, raised thumb ---- */
        ".buffer-scrollbar, .buffer-hscrollbar {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"
        ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
        "  background: #%06x;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"

        /* ---- Floating surfaces and chrome: raised cards ---- */
        ".ca-popup-card, .ca-select-popup, .ca-context-menu,"
        ".ca-menubar-popup, .cf-panel, .fp-root, .search-root-window,"
        ".pm-root, .sw-root, .pm-left, .sw-left, .fp-toolbar, .fp-footer,"
        ".fp-colhdr, .search-header, .search-footer, .pm-search-row,"
        ".scm-toolbar, .scm-repository, .scm-commit-box, .scm-section-header,"
        ".ca-titlebar {"
        "  background: #%06x;"
        "  border-top-width: 2px; border-left-width: 2px;"
        "  border-bottom-width: 2px; border-right-width: 2px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        /* Status bar: same raised bevel as the project tab strip above —
           full width, flush to the bottom edge, the mirror of the strip
           at the top. The glass theme floats it as an inset pill
           (width:auto + side/bottom margins + pill radius), which has to
           be reset here — a bevelled pill contradicts the relief
           language, and an inset bottom bar against a flush top strip
           reads as an alignment bug. Previously only a single top
           hairline (no bevel at all), which read flat next to the tab
           strip's proper raised relief. */
        ".status-bar {"
        "  background: #%06x;"
        "  width: 100%%;"
        "  margin: 8px 0px 0px 0px;"
        "  padding: 0px 8px;"
        "  flex-grow: 0; flex-shrink: 0;"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"
        /* Hard-edged shadow rather than a soft blur: a gaussian falloff
           is the one detail that instantly dates this as a re-creation. */
        ".ca-popup-card, .ca-select-popup, .ca-context-menu,"
        ".ca-menubar-popup, .cf-panel, .term-float-panel {"
        "  shadow-offset-y: 2px;"
        "  shadow-blur: 0px;"
        "  shadow-color: rgba(0, 0, 0, 0.55);"
        "}"

        /* ---- Rows: flat by default, sunken once selected ---- */
        ".tree-row, .tree-sticky-row, .fp-row, .search-result, .pm-item,"
        ".sess-item, .scm-file-row, .scm-commit-row, .scm-branch-row,"
        ".cf-row {"
        "  border-radius: 0px; border-width: 0px;"
        "}"
        ".fp-row-selected, .search-result-selected, .pm-item-selected,"
        ".sess-item-selected, .scm-branch-row-current {"
        "  border-top-width: 1px; border-left-width: 1px;"
        "  border-bottom-width: 1px; border-right-width: 1px;"
        "  border-top-color: #%06x; border-left-color: #%06x;"
        "  border-bottom-color: #%06x; border-right-color: #%06x;"
        "  border-radius: 0px;"
        "}"

        /* ---- Separators become a chiselled groove ---- */
        ".sw-hr, .pm-hr, .welcome-hr {"
        "  border-top-width: 1px; border-bottom-width: 1px;"
        "  border-top-color: #%06x; border-bottom-color: #%06x;"
        "  border-radius: 0px;"
        "  background: transparent;"
        "}",

        /* native overlay scrollbars */
        well, surface, pressed,
        /* panels: raised frame on the widget surface */
        surface, light, light, dark, dark,
        /* wells */
        well, well_dark, well_dark, well_light, well_light,
        /* tab strip: raised */
        surface, light, light, dark, dark,
        /* active tab: raised, sitting on the strip */
        surface, light, light, dark, dark,
        /* buttons: raised */
        surface, light, light, dark, dark,
        /* buttons pressed: inverted + darker face */
        pressed, dark, dark, light, light,
        /* inputs: sunken well */
        well, well_dark, well_dark, well_light, well_light,
        /* scrollbar trough: sunken */
        well, well_dark, well_dark, well_light, well_light,
        /* scrollbar thumb: raised */
        surface, light, light, dark, dark,
        /* floating cards + titlebar: raised */
        surface, light, light, dark, dark,
        /* status bar: raised, same relief as the tab strip */
        surface, light, light, dark, dark,
        /* selected rows: sunken */
        dark, dark, light, light,
        /* separator groove */
        dark, light);

    if (n < 0 || n >= bufsz) return 0;
    return n;
}

#endif /* SOL_UI_STYLE_RETRO_H */
