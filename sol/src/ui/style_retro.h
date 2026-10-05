// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* Retro widget geometry. Themes own every color; this overlay owns only
 * shape, relief geometry, spacing, and blur/shadow form. */

#ifndef SOL_UI_STYLE_RETRO_H
#define SOL_UI_STYLE_RETRO_H

#include "sol_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOL_UI_STYLE_RETRO_CSS \
    "/* ===== Retro style: geometry only ===== */" \
    "*{border-radius:0px;backdrop-filter:blur(0px);}" \
    ".native-scrollbar{scrollbar-radius:0px;scrollbar-track-border-width:1px;scrollbar-thumb-border-width:1px;}" \
    ".workspace-panel,.tree-panel,.plugin-side-panel,.buffer-pane,.term-panel,.welcome-pane,.term-float-panel{" \
    "border-top-width:2px;border-left-width:2px;border-bottom-width:2px;border-right-width:2px;border-radius:0px;}" \
    ".tree-panel-focused,.plugin-side-panel-focused,.buffer-pane-focused,.term-panel-focused{border-width:1px;border-radius:0px;}" \
    ".workspace-panel-well,.buffer-body,.buffer-scroll-row,.term-viewport,.term-filler,.tree-scroll-area,.scm-root," \
    ".pm-list,.fp-list,.search-results,.pm-right,.sw-right,.workspace-panel-chrome,.buffer-tabs-row,.term-header," \
    ".project-tabs,.scm-header,.scm-tabs,.scm-section-header{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;border-radius:0px;}" \
    ".buffer-tab,.term-tab,.project-tab,.scm-tab,.sw-tab-btn,.toolbar-session-row,.toolbar-session-item{" \
    "border-radius:0px;border-width:0px;}" \
    ".buffer-tab-active,.term-tab-active,.project-tab-active,.scm-tab-active,.sw-tab-btn-active{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;border-radius:0px;}" \
    ".ca-popup-btn,.ca-titlebar-control,.ca-titlebar-menu-item,.pm-btn,.pm-btn-enable,.pm-btn-disable," \
    ".fp-up-btn,.fp-crumb-btn,.fp-colhdr-btn,.fp-colhdr-btn-end,.fp-action-cancel,.fp-action-primary," \
    ".fp-action-new-folder,.fp-nf-create,.fp-nf-cancel,.welcome-btn,.welcome-btn-primary,.welcome-recent-session," \
    ".scm-header-action,.scm-action,.scm-icon-action,.scm-header-icon-action,.scm-header-close-action,.scm-action-icon," \
    ".scm-branch-create-from,.scm-section-action,.scm-primary-action,.scm-danger-action,.scm-remote-action," \
    ".buffer-tab-close,.term-tab-close,.term-tab-new,.project-tab-close,.project-tab-new,.status-bar-badge,.cf-row-key,.pm-badge,.sw-btn," \
    ".toolbar-session-trigger,.toolbar-session-close,.toolbar-icon,.toolbar-cmake-group,.toolbar-target-select," \
    ".fp-new-folder-input,.search-input,.pm-search-input,.sw-scale-input,.sw-select,.sw-bind-input," \
    ".scm-commit-input,.scm-branch-input,.buffer-scrollbar,.buffer-hscrollbar," \
    ".buffer-scrollbar-thumb,.buffer-scrollbar-thumb-active,.buffer-hscrollbar-thumb,.buffer-hscrollbar-thumb-active{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;border-radius:0px;}" \
    ".ca-popup-btn:active,.ca-titlebar-control:active,.ca-titlebar-menu-item:active,.pm-btn:active,.pm-btn-enable:active," \
    ".pm-btn-disable:active,.fp-up-btn:active,.fp-crumb-btn:active,.fp-action-cancel:active,.fp-action-primary:active," \
    ".fp-action-new-folder:active,.fp-nf-create:active,.fp-nf-cancel:active,.welcome-btn:active,.welcome-btn-primary:active," \
    ".scm-action:active,.scm-primary-action:active,.scm-danger-action:active,.buffer-tab-close:active," \
    ".term-tab-close:active,.term-tab-new:active,.project-tab-close:active,.project-tab-new:active,.sw-btn:active{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;}" \
    ".toolbar-session-trigger:active,.toolbar-session-close:active,.toolbar-icon:active{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;}" \
    ".ca-popup-card,.ca-select-popup,.ca-context-menu,.ca-menubar-popup,.cf-panel,.fp-root,.search-root-window," \
    ".pm-root,.sw-root,.pm-left,.sw-left,.fp-toolbar,.fp-footer,.fp-colhdr,.search-header,.search-footer,.pm-search-row," \
    ".scm-toolbar,.scm-repository,.scm-commit-box,.scm-section-header,.ca-titlebar,.toolbar-session-menu{" \
    "border-top-width:2px;border-left-width:2px;border-bottom-width:2px;border-right-width:2px;border-radius:0px;}" \
    ".status-bar{width:100%;margin:8px 0px 0px 0px;padding:0px 8px;flex-grow:0;flex-shrink:0;" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;border-radius:0px;}" \
    ".ca-popup-card,.ca-select-popup,.ca-context-menu,.ca-menubar-popup,.cf-panel,.term-float-panel,.toolbar-session-menu{" \
    "shadow-offset-y:2px;shadow-blur:0px;}" \
    ".tree-row,.tree-sticky-row,.fp-row,.search-result,.pm-item,.sess-item,.scm-file-row,.scm-commit-row," \
    ".scm-branch-row,.cf-row{border-radius:0px;border-width:0px;}" \
    ".fp-row-selected,.search-result-selected,.pm-item-selected,.sess-item-selected,.scm-branch-row-current," \
    ".toolbar-session-row-active," \
    ".scm-submodule-row,.scm-submodule-card-clean,.scm-submodule-card-modified,.scm-submodule-card-warning," \
    ".scm-submodule-card-conflict,.scm-submodule-card-clean:hover,.scm-submodule-card-modified:hover," \
    ".scm-submodule-card-warning:hover,.scm-submodule-card-conflict:hover,.scm-submodule-row:active," \
    ".scm-tag,.scm-tag-branch,.scm-tag-clean,.scm-tag-modified,.scm-tag-warning,.scm-tag-conflict{" \
    "border-top-width:1px;border-left-width:1px;border-bottom-width:1px;border-right-width:1px;border-radius:0px;}" \
    ".sw-hr,.pm-hr,.welcome-hr{border-top-width:1px;border-bottom-width:1px;border-radius:0px;}"

/* Blend packed RGB colors using an 8-bit weight for b. */
static inline uint32_t sol_retro_mix(uint32_t a, uint32_t b, unsigned weight)
{
    const unsigned inverse = 255u - weight;
    const unsigned ar = (a >> 16u) & 0xffu, ag = (a >> 8u) & 0xffu, ab = a & 0xffu;
    const unsigned br = (b >> 16u) & 0xffu, bg = (b >> 8u) & 0xffu, bb = b & 0xffu;
    return (((ar * inverse + br * weight) / 255u) << 16u) |
           (((ag * inverse + bg * weight) / 255u) << 8u) |
           ((ab * inverse + bb * weight) / 255u);
}

/*
 * Build Retro geometry plus theme-derived relief colors.
 *
 * colors  Active theme palette anchors.
 * Returns a heap CSS string owned by the caller, or NULL on failure.
 */
static inline char *sol_retro_build_css(const SolThemeColors *colors)
{
    if (!colors) return NULL;
    const uint32_t highlight = sol_retro_mix(colors->background_rgb,
                                              colors->primary_rgb, 112u);
    const uint32_t shadow = sol_retro_mix(highlight, 0x000000u, 176u);
    const char *edge_selectors =
        ".workspace-panel,.tree-panel,.plugin-side-panel,.buffer-pane,.term-panel,.welcome-pane,.term-float-panel,"
        ".workspace-panel-well,.buffer-body,.buffer-scroll-row,.term-viewport,.term-filler,.tree-scroll-area,.scm-root,"
        ".pm-list,.fp-list,.search-results,.pm-right,.sw-right,.workspace-panel-chrome,.buffer-tabs-row,.term-header,"
        ".project-tabs,.scm-header,.scm-tabs,.scm-section-header,.buffer-tab-active,.term-tab-active,.project-tab-active,"
        ".scm-tab-active,.sw-tab-btn-active,.ca-popup-btn,.ca-titlebar-control,.ca-titlebar-menu-item,.pm-btn,.pm-btn-enable,"
        ".pm-btn-disable,.fp-up-btn,.fp-crumb-btn,.fp-colhdr-btn,.fp-colhdr-btn-end,.fp-action-cancel,.fp-action-primary,"
        ".fp-action-new-folder,.fp-nf-create,.fp-nf-cancel,.welcome-btn,.welcome-btn-primary,.welcome-recent-session,"
        ".scm-header-action,.scm-action,.scm-icon-action,.scm-header-icon-action,.scm-header-close-action,.scm-action-icon,"
        ".scm-branch-create-from,.scm-section-action,.scm-primary-action,.scm-danger-action,.scm-remote-action,"
        ".buffer-tab-close,.term-tab-close,.term-tab-new,.project-tab-close,.project-tab-new,.status-bar-badge,.cf-row-key,.pm-badge,.sw-btn,"
        ".toolbar-session-trigger,.toolbar-session-close,.toolbar-icon,.toolbar-cmake-group,.toolbar-target-select,"
        ".fp-new-folder-input,.search-input,.pm-search-input,.sw-scale-input,.sw-select,.sw-bind-input,.scm-commit-input,"
        ".scm-branch-input,.buffer-scrollbar,.buffer-hscrollbar,.buffer-scrollbar-thumb,.buffer-scrollbar-thumb-active,"
        ".buffer-hscrollbar-thumb,.buffer-hscrollbar-thumb-active,.ca-popup-card,.ca-select-popup,.ca-context-menu,"
        ".ca-menubar-popup,.cf-panel,.fp-root,.search-root-window,.pm-root,.sw-root,.pm-left,.sw-left,.fp-toolbar,.fp-footer,.toolbar-session-menu,"
        ".fp-colhdr,.search-header,.search-footer,.pm-search-row,.scm-toolbar,.scm-repository,.scm-commit-box,.ca-titlebar,"
        ".status-bar,.fp-row-selected,.search-result-selected,.pm-item-selected,.sess-item-selected,.scm-branch-row-current,.toolbar-session-row-active,"
        ".scm-submodule-row,.scm-submodule-card-clean,.scm-submodule-card-modified,.scm-submodule-card-warning,"
        ".scm-submodule-card-conflict,.scm-tag,.scm-tag-branch,.scm-tag-clean,.scm-tag-modified,.scm-tag-warning,.scm-tag-conflict";
    const char *format = "%s%s{border-top-color:#%06x;border-left-color:#%06x;"
                         "border-bottom-color:#%06x;border-right-color:#%06x;}"
                         ".native-scrollbar{scrollbar-track-border-top-color:#%06x;"
                         "scrollbar-track-border-left-color:#%06x;scrollbar-track-border-bottom-color:#%06x;"
                         "scrollbar-track-border-right-color:#%06x;scrollbar-thumb-border-top-color:#%06x;"
                         "scrollbar-thumb-border-left-color:#%06x;scrollbar-thumb-border-bottom-color:#%06x;"
                         "scrollbar-thumb-border-right-color:#%06x;}"
                         ".tree-panel-focused,.plugin-side-panel-focused,.buffer-pane-focused,.term-panel-focused"
                         "{border-color:#%06x;}";
    const int length = snprintf(NULL, 0, format, SOL_UI_STYLE_RETRO_CSS, edge_selectors,
                                highlight, highlight, shadow, shadow,
                                shadow, shadow, highlight, highlight,
                                highlight, highlight, shadow, shadow,
                                colors->primary_rgb);
    if (length < 0) return NULL;
    char *css = (char *)malloc((size_t)length + 1u);
    if (!css) return NULL;
    snprintf(css, (size_t)length + 1u, format, SOL_UI_STYLE_RETRO_CSS, edge_selectors,
             highlight, highlight, shadow, shadow,
             shadow, shadow, highlight, highlight,
             highlight, highlight, shadow, shadow,
             colors->primary_rgb);
    return css;
}

#endif /* SOL_UI_STYLE_RETRO_H */
