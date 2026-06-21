// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-themes — Built-in editor themes for Sol.
 *
 * Registers four CSS override themes that extend the default Glass baseline:
 *
 *   com.sol.theme.ocean      — deep teal/cyan palette
 *   com.sol.theme.amethyst   — purple/violet palette
 *   com.sol.theme.graphite   — neutral monochrome palette
 *   com.sol.theme.ember      — warm amber/rust palette
 *
 * Each override targets every major visual surface:
 *   - Chrome (title bar, status bar, splitters)
 *   - Panels (file tree, side panel, plugin manager, settings)
 *   - Editor (buffer body, gutter, tabs, scrollbars, selection, caret)
 *   - Syntax highlighting tokens
 *   - Auxiliary windows (file picker, search, welcome, terminal)
 */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "sol_theme.h"

/* ------------------------------------------------------------------ */
/* Theme CSS overrides                                                 */
/* ------------------------------------------------------------------ */

/*
 * Deep Ocean — teal and cyan tones, cool deep-sea atmosphere.
 */
static const char k_ocean_css[] =
    /* Chrome */
    ".ca-titlebar { background: rgba(2, 10, 20, 0.96); }"
    ".ca-titlebar-title { color: #4a8fa8; }"
    ".ca-titlebar-menu-item { color: #7ab8cc; }"
    ".ca-titlebar-menu-item:hover { background: rgba(64, 160, 190, 0.14); color: #b0dde8; }"
    ".ca-titlebar-control { color: #4a8fa8; }"
    ".ca-titlebar-control:hover { background: rgba(64, 160, 190, 0.12); }"
    ".ca-titlebar-close { color: #60a8c0; }"
    ".ca-titlebar-close:hover { background: rgba(60, 140, 180, 0.22); }"
    "splitter { background: transparent; color: rgba(64, 184, 210, 0.60); }"
    /* Status bar */
    ".status-bar { background: rgba(2, 10, 20, 0.97); }"
    ".status-bar-text { color: #4a8fa8; }"
    ".status-bar-badge-key { background: rgba(48, 144, 184, 0.34); }"
    ".status-bar-badge-command { background: rgba(40, 160, 140, 0.30); }"
    ".status-bar-badge-leader { background: rgba(160, 120, 48, 0.30); }"
    /* Panels */
    ".tree-panel, .plugin-side-panel { background: rgba(3, 14, 24, 0.92); }"
    ".tree-section-header { background: transparent; }"
    ".tree-section-title { color: #4a8fa8; }"
    ".tree-row:hover { background: rgba(64, 184, 210, 0.10); }"
    ".tree-sticky-row { background: rgba(4, 18, 30, 0.94); }"
    ".tree-sticky-row:hover { background: rgba(64, 184, 210, 0.12); }"
    ".tree-arrow { color: #2a6080; }"
    ".tree-arrow-open { color: #40b8d0; }"
    ".tree-name { color: #8ab8cc; }"
    ".tree-name-dir { color: #c0dde8; }"
    /* Buffer / editor */
    ".buffer-pane, .buffer-pane-active { background: transparent; }"
    ".buffer-tabs-row { background: rgba(3, 12, 22, 0.86); }"
    ".buffer-tab { background: transparent; corner-radius: 0px; }"
    ".buffer-tab:hover { background: rgba(48, 160, 188, 0.12); }"
    ".buffer-tab-active { background: rgba(24, 78, 100, 0.88); }"
    ".buffer-tab-text { color: #3a7a94; }"
    ".buffer-tab-text-active { color: #d0eef6; }"
    ".buffer-tab-close:hover { background: rgba(60, 140, 180, 0.20); }"
    ".buffer-body { background: rgba(3, 10, 18, 0.92); }"
    ".buffer-gutter-col { background: rgba(4, 12, 20, 0.84); }"
    ".buffer-gutter-line { color: #2a5868; }"
    ".buffer-line, .buffer-body-text, .hl-plain { color: #c8dce4; }"
    ".buffer-selection { background: rgba(20, 88, 112, 0.88); }"
    ".buffer-caret { background: #40c0d8; }"
    ".buffer-scrollbar { background: rgba(4, 12, 20, 0.30); }"
    ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(48, 144, 172, 0.38); corner-radius: 0px;"
    "}"
    ".buffer-scrollbar-thumb:hover, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb:hover, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(64, 172, 204, 0.56);"
    "}"
    /* Syntax */
    ".hl-keyword, .hl-macro { color: #56c8e0; }"
    ".hl-comment { color: #3a7888; }"
    ".hl-string, .hl-regex { color: #80d8a0; }"
    ".hl-number, .hl-constant, .hl-escape { color: #7ad4c0; }"
    ".hl-function, .hl-constructor { color: #60b8f0; }"
    ".hl-type, .hl-namespace { color: #40cce0; }"
    ".hl-property, .hl-attribute, .hl-parameter { color: #90c8d8; }"
    ".hl-operator, .hl-label { color: #60b0c8; }"
    ".hl-tag { color: #e06878; }"
    /* Welcome */
    ".welcome-pane { background: rgba(3, 10, 18, 0.90); }"
    ".welcome-title { color: #b0dde8; }"
    ".welcome-subtitle, .welcome-section-label { color: #3a7888; }"
    ".welcome-hr { background: rgba(48, 144, 172, 0.24); }"
    ".welcome-btn { background: rgba(48, 130, 160, 0.12); color: #90c0d0; }"
    ".welcome-btn-primary { background: rgba(32, 110, 150, 0.28); color: #c8eaf4; }"
    ".welcome-btn:hover, .welcome-btn-primary:hover { background: rgba(48, 144, 184, 0.26); }"
    /* Aux windows */
    ".fp-root, .search-root-window { background: rgba(4, 12, 22, 0.90); }"
    ".fp-toolbar, .fp-footer, .fp-colhdr, .search-header, .search-footer {"
    "  background: rgba(6, 18, 32, 0.92);"
    "}"
    ".fp-list, .search-results { background: rgba(3, 10, 18, 0.88); }"
    ".fp-row:hover, .search-result:hover { background: rgba(48, 160, 188, 0.10); }"
    ".fp-row-selected, .search-result-selected { background: rgba(20, 88, 120, 0.88); }"
    ".fp-new-folder-input, .search-input { background: rgba(3, 10, 18, 0.88); corner-radius: 0px; color: #c8dce4; }"
    ".search-result-line { color: #40b8d0; }"
    /* Plugin/Settings windows */
    ".pm-root, .sw-root { background: rgba(4, 12, 22, 0.90); }"
    ".pm-left, .sw-left { background: rgba(3, 14, 26, 0.88); }"
    ".pm-item:hover, .sw-tab-btn:hover { background: rgba(48, 160, 188, 0.10); }"
    ".pm-item-selected, .sw-tab-btn-active { background: rgba(20, 88, 120, 0.80); }"
    ".pm-right, .sw-right { background: rgba(4, 10, 18, 0.60); }"
    ".sw-hr, .pm-hr { background: #1a4c60; }"
    ".pm-btn, .pm-btn-enable, .pm-btn-disable, .sw-effect-btn { background: rgba(40, 130, 160, 0.14); corner-radius: 0px; }"
    ".pm-btn:hover, .sw-effect-btn:hover { background: rgba(48, 150, 184, 0.22); }"
    ".sw-effect-btn-active { background: rgba(24, 100, 140, 0.34); }"
    /* Command flow popup */
    ".cf-panel { background: rgba(3, 12, 22, 0.95); corner-radius: 0px; }"
    ".cf-row-key { background: rgba(48, 144, 184, 0.16); corner-radius: 0px; }"
    ".cf-row-key-text { color: #40c0d8; }"
    /* Terminal */
    ".term-panel, .term-viewport, .term-filler { background: rgba(3, 10, 18, 0.96); }"
    ".term-header { background: rgba(4, 14, 24, 0.96); }"
    ".term-tab { background: rgba(4, 14, 24, 0.96); color: #3a7888; }"
    ".term-tab-active { background: rgba(3, 10, 18, 0.96); color: #a0d0dc; }"
    ".term-cursor-focused { background: #40c0d8; color: #02080e; }";

/*
 * Amethyst — purple and violet tones, rich jewel-toned atmosphere.
 */
static const char k_amethyst_css[] =
    /* Chrome */
    ".ca-titlebar { background: rgba(12, 5, 22, 0.97); }"
    ".ca-titlebar-title { color: #8a5ab8; }"
    ".ca-titlebar-menu-item { color: #a878d8; }"
    ".ca-titlebar-menu-item:hover { background: rgba(140, 80, 200, 0.14); color: #d0a8f0; }"
    ".ca-titlebar-control { color: #8a5ab8; }"
    ".ca-titlebar-control:hover { background: rgba(140, 80, 200, 0.12); }"
    ".ca-titlebar-close { color: #c060a0; }"
    ".ca-titlebar-close:hover { background: rgba(180, 60, 120, 0.22); }"
    "splitter { background: transparent; color: rgba(176, 100, 220, 0.60); }"
    /* Status bar */
    ".status-bar { background: rgba(10, 4, 20, 0.98); }"
    ".status-bar-text { color: #8a5ab8; }"
    ".status-bar-badge-key { background: rgba(120, 72, 196, 0.34); }"
    ".status-bar-badge-command { background: rgba(80, 100, 160, 0.30); }"
    ".status-bar-badge-leader { background: rgba(160, 100, 48, 0.30); }"
    /* Panels */
    ".tree-panel, .plugin-side-panel { background: rgba(14, 6, 26, 0.92); }"
    ".tree-section-header { background: transparent; }"
    ".tree-section-title { color: #8a5ab8; }"
    ".tree-row:hover { background: rgba(140, 80, 200, 0.10); }"
    ".tree-sticky-row { background: rgba(18, 8, 34, 0.95); }"
    ".tree-sticky-row:hover { background: rgba(140, 80, 200, 0.12); }"
    ".tree-arrow { color: #5a3880; }"
    ".tree-arrow-open { color: #b068e0; }"
    ".tree-name { color: #c0a0dc; }"
    ".tree-name-dir { color: #e0c8f4; }"
    /* Buffer / editor */
    ".buffer-pane, .buffer-pane-active { background: transparent; }"
    ".buffer-tabs-row { background: rgba(12, 5, 22, 0.86); }"
    ".buffer-tab { background: transparent; corner-radius: 0px; }"
    ".buffer-tab:hover { background: rgba(120, 64, 180, 0.12); }"
    ".buffer-tab-active { background: rgba(60, 28, 90, 0.90); }"
    ".buffer-tab-text { color: #6a3a90; }"
    ".buffer-tab-text-active { color: #e8d0f8; }"
    ".buffer-tab-close:hover { background: rgba(180, 60, 120, 0.20); }"
    ".buffer-body { background: rgba(10, 4, 18, 0.92); }"
    ".buffer-gutter-col { background: rgba(14, 6, 24, 0.84); }"
    ".buffer-gutter-line { color: #3a2258; }"
    ".buffer-line, .buffer-body-text, .hl-plain { color: #dcd0e8; }"
    ".buffer-selection { background: rgba(80, 36, 120, 0.88); }"
    ".buffer-caret { background: #c080e8; }"
    ".buffer-scrollbar { background: rgba(14, 6, 24, 0.30); }"
    ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(120, 72, 180, 0.38); corner-radius: 0px;"
    "}"
    ".buffer-scrollbar-thumb:hover, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb:hover, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(150, 96, 220, 0.56);"
    "}"
    /* Syntax */
    ".hl-keyword, .hl-macro { color: #c890f0; }"
    ".hl-comment { color: #5a3878; }"
    ".hl-string, .hl-regex { color: #a8d870; }"
    ".hl-number, .hl-constant, .hl-escape { color: #e8b878; }"
    ".hl-function, .hl-constructor { color: #90b0f8; }"
    ".hl-type, .hl-namespace { color: #80e0d0; }"
    ".hl-property, .hl-attribute, .hl-parameter { color: #c0b0e0; }"
    ".hl-operator, .hl-label { color: #a890d0; }"
    ".hl-tag { color: #f07890; }"
    /* Welcome */
    ".welcome-pane { background: rgba(10, 4, 18, 0.90); }"
    ".welcome-title { color: #d8b8f8; }"
    ".welcome-subtitle, .welcome-section-label { color: #6a3a90; }"
    ".welcome-hr { background: rgba(120, 72, 180, 0.24); }"
    ".welcome-btn { background: rgba(100, 60, 150, 0.12); color: #c0a0e0; }"
    ".welcome-btn-primary { background: rgba(80, 40, 130, 0.28); color: #e0c8f8; }"
    ".welcome-btn:hover, .welcome-btn-primary:hover { background: rgba(120, 72, 180, 0.26); }"
    /* Aux windows */
    ".fp-root, .search-root-window { background: rgba(12, 5, 20, 0.90); }"
    ".fp-toolbar, .fp-footer, .fp-colhdr, .search-header, .search-footer {"
    "  background: rgba(16, 8, 28, 0.92);"
    "}"
    ".fp-list, .search-results { background: rgba(10, 4, 18, 0.88); }"
    ".fp-row:hover, .search-result:hover { background: rgba(120, 64, 180, 0.10); }"
    ".fp-row-selected, .search-result-selected { background: rgba(60, 28, 100, 0.88); }"
    ".fp-new-folder-input, .search-input { background: rgba(10, 4, 18, 0.88); corner-radius: 0px; color: #dcd0e8; }"
    ".search-result-line { color: #c080e8; }"
    /* Plugin/Settings windows */
    ".pm-root, .sw-root { background: rgba(12, 5, 22, 0.90); }"
    ".pm-left, .sw-left { background: rgba(14, 6, 26, 0.88); }"
    ".pm-item:hover, .sw-tab-btn:hover { background: rgba(120, 64, 180, 0.10); }"
    ".pm-item-selected, .sw-tab-btn-active { background: rgba(60, 28, 100, 0.80); }"
    ".pm-right, .sw-right { background: rgba(10, 4, 18, 0.60); }"
    ".sw-hr, .pm-hr { background: #3a1c60; }"
    ".pm-btn, .pm-btn-enable, .pm-btn-disable, .sw-effect-btn { background: rgba(110, 60, 170, 0.14); corner-radius: 0px; }"
    ".pm-btn:hover, .sw-effect-btn:hover { background: rgba(130, 80, 200, 0.22); }"
    ".sw-effect-btn-active { background: rgba(80, 36, 130, 0.34); }"
    /* Command flow popup */
    ".cf-panel { background: rgba(10, 4, 20, 0.96); corner-radius: 0px; }"
    ".cf-row-key { background: rgba(120, 72, 196, 0.16); corner-radius: 0px; }"
    ".cf-row-key-text { color: #c080e8; }"
    /* Terminal */
    ".term-panel, .term-viewport, .term-filler { background: rgba(10, 4, 18, 0.96); }"
    ".term-header { background: rgba(14, 6, 26, 0.96); }"
    ".term-tab { background: rgba(14, 6, 26, 0.96); color: #6a3a90; }"
    ".term-tab-active { background: rgba(10, 4, 18, 0.96); color: #d0b0f0; }"
    ".term-cursor-focused { background: #c080e8; color: #080410; }";

/*
 * Graphite — neutral grays, high-contrast monochrome focus.
 */
static const char k_graphite_css[] =
    /* Chrome */
    ".ca-titlebar { background: rgba(8, 8, 10, 0.98); }"
    ".ca-titlebar-title { color: #707880; }"
    ".ca-titlebar-menu-item { color: #909aa4; }"
    ".ca-titlebar-menu-item:hover { background: rgba(140, 148, 160, 0.12); color: #c8d0d8; }"
    ".ca-titlebar-control { color: #606870; }"
    ".ca-titlebar-control:hover { background: rgba(140, 148, 160, 0.10); }"
    ".ca-titlebar-close { color: #c07070; }"
    ".ca-titlebar-close:hover { background: rgba(200, 80, 80, 0.18); }"
    "splitter { background: transparent; color: rgba(160, 168, 178, 0.50); }"
    /* Status bar */
    ".status-bar { background: rgba(5, 5, 7, 0.98); }"
    ".status-bar-text { color: #606870; }"
    ".status-bar-badge-key { background: rgba(100, 110, 125, 0.32); }"
    ".status-bar-badge-command { background: rgba(80, 110, 90, 0.28); }"
    ".status-bar-badge-leader { background: rgba(130, 110, 60, 0.28); }"
    /* Panels */
    ".tree-panel, .plugin-side-panel { background: rgba(10, 10, 13, 0.93); }"
    ".tree-section-header { background: transparent; }"
    ".tree-section-title { color: #606870; }"
    ".tree-row:hover { background: rgba(140, 148, 160, 0.08); }"
    ".tree-sticky-row { background: rgba(12, 12, 16, 0.95); }"
    ".tree-sticky-row:hover { background: rgba(140, 148, 160, 0.10); }"
    ".tree-arrow { color: #484e56; }"
    ".tree-arrow-open { color: #909aa4; }"
    ".tree-name { color: #a0a8b0; }"
    ".tree-name-dir { color: #c8d0d8; }"
    /* Buffer / editor */
    ".buffer-pane, .buffer-pane-active { background: transparent; }"
    ".buffer-tabs-row { background: rgba(8, 8, 10, 0.86); }"
    ".buffer-tab { background: transparent; corner-radius: 0px; }"
    ".buffer-tab:hover { background: rgba(120, 128, 138, 0.10); }"
    ".buffer-tab-active { background: rgba(44, 46, 52, 0.94); }"
    ".buffer-tab-text { color: #4a5058; }"
    ".buffer-tab-text-active { color: #e8ecf0; }"
    ".buffer-tab-close:hover { background: rgba(200, 80, 80, 0.16); }"
    ".buffer-body { background: rgba(8, 8, 10, 0.93); }"
    ".buffer-gutter-col { background: rgba(10, 10, 13, 0.86); }"
    ".buffer-gutter-line { color: #2e3238; }"
    ".buffer-line, .buffer-body-text, .hl-plain { color: #d8dce0; }"
    ".buffer-selection { background: rgba(54, 58, 66, 0.92); }"
    ".buffer-caret { background: #c8d0d8; }"
    ".buffer-scrollbar { background: rgba(10, 10, 13, 0.28); }"
    ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(100, 108, 118, 0.38); corner-radius: 0px;"
    "}"
    ".buffer-scrollbar-thumb:hover, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb:hover, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(130, 140, 152, 0.54);"
    "}"
    /* Syntax — desaturated but readable */
    ".hl-keyword, .hl-macro { color: #c0c8d0; }"
    ".hl-comment { color: #505860; }"
    ".hl-string, .hl-regex { color: #a8b8a0; }"
    ".hl-number, .hl-constant, .hl-escape { color: #b8b090; }"
    ".hl-function, .hl-constructor { color: #b0c0d0; }"
    ".hl-type, .hl-namespace { color: #98b8c8; }"
    ".hl-property, .hl-attribute, .hl-parameter { color: #a8b0bc; }"
    ".hl-operator, .hl-label { color: #909aa4; }"
    ".hl-tag { color: #c89090; }"
    /* Welcome */
    ".welcome-pane { background: rgba(8, 8, 10, 0.92); }"
    ".welcome-title { color: #d8dce0; }"
    ".welcome-subtitle, .welcome-section-label { color: #505860; }"
    ".welcome-hr { background: rgba(100, 108, 118, 0.22); }"
    ".welcome-btn { background: rgba(100, 108, 118, 0.10); color: #a0a8b0; }"
    ".welcome-btn-primary { background: rgba(70, 78, 90, 0.26); color: #d8dce0; }"
    ".welcome-btn:hover, .welcome-btn-primary:hover { background: rgba(110, 120, 132, 0.24); }"
    /* Aux windows */
    ".fp-root, .search-root-window { background: rgba(8, 8, 10, 0.92); }"
    ".fp-toolbar, .fp-footer, .fp-colhdr, .search-header, .search-footer {"
    "  background: rgba(12, 12, 16, 0.94);"
    "}"
    ".fp-list, .search-results { background: rgba(8, 8, 10, 0.90); }"
    ".fp-row:hover, .search-result:hover { background: rgba(110, 118, 128, 0.09); }"
    ".fp-row-selected, .search-result-selected { background: rgba(44, 48, 56, 0.90); }"
    ".fp-new-folder-input, .search-input { background: rgba(8, 8, 10, 0.90); corner-radius: 0px; color: #d8dce0; }"
    ".search-result-line { color: #9090a0; }"
    /* Plugin/Settings windows */
    ".pm-root, .sw-root { background: rgba(8, 8, 12, 0.92); }"
    ".pm-left, .sw-left { background: rgba(10, 10, 14, 0.90); }"
    ".pm-item:hover, .sw-tab-btn:hover { background: rgba(110, 118, 128, 0.09); }"
    ".pm-item-selected, .sw-tab-btn-active { background: rgba(44, 48, 56, 0.88); }"
    ".pm-right, .sw-right { background: rgba(8, 8, 10, 0.60); }"
    ".sw-hr, .pm-hr { background: #282e36; }"
    ".pm-btn, .pm-btn-enable, .pm-btn-disable, .sw-effect-btn { background: rgba(90, 98, 108, 0.13); corner-radius: 0px; }"
    ".pm-btn:hover, .sw-effect-btn:hover { background: rgba(110, 120, 132, 0.20); }"
    ".sw-effect-btn-active { background: rgba(60, 66, 76, 0.36); }"
    /* Command flow popup */
    ".cf-panel { background: rgba(8, 8, 12, 0.97); corner-radius: 0px; }"
    ".cf-row-key { background: rgba(90, 100, 112, 0.16); corner-radius: 0px; }"
    ".cf-row-key-text { color: #a0a8b4; }"
    /* Terminal */
    ".term-panel, .term-viewport, .term-filler { background: rgba(8, 8, 10, 0.97); }"
    ".term-header { background: rgba(10, 10, 14, 0.97); }"
    ".term-tab { background: rgba(10, 10, 14, 0.97); color: #505860; }"
    ".term-tab-active { background: rgba(8, 8, 10, 0.97); color: #c8d0d8; }"
    ".term-cursor-focused { background: #c8d0d8; color: #080810; }";

/*
 * Ember Glass — warm amber, rust, and orange tones.
 */
static const char k_ember_css[] =
    /* Chrome */
    ".ca-titlebar { background: rgba(18, 8, 4, 0.97); }"
    ".ca-titlebar-title { color: #a06038; }"
    ".ca-titlebar-menu-item { color: #c88050; }"
    ".ca-titlebar-menu-item:hover { background: rgba(190, 110, 50, 0.14); color: #f0c090; }"
    ".ca-titlebar-control { color: #a06038; }"
    ".ca-titlebar-control:hover { background: rgba(190, 110, 50, 0.12); }"
    ".ca-titlebar-close { color: #e07040; }"
    ".ca-titlebar-close:hover { background: rgba(220, 80, 50, 0.22); }"
    "splitter { background: transparent; color: rgba(220, 140, 70, 0.60); }"
    /* Status bar */
    ".status-bar { background: rgba(14, 6, 2, 0.98); }"
    ".status-bar-text { color: #906030; }"
    ".status-bar-badge-key { background: rgba(180, 110, 48, 0.34); }"
    ".status-bar-badge-command { background: rgba(100, 130, 70, 0.28); }"
    ".status-bar-badge-leader { background: rgba(180, 80, 48, 0.30); }"
    /* Panels */
    ".tree-panel, .plugin-side-panel { background: rgba(22, 10, 5, 0.92); }"
    ".tree-section-header { background: transparent; }"
    ".tree-section-title { color: #905530; }"
    ".tree-row:hover { background: rgba(190, 110, 50, 0.10); }"
    ".tree-sticky-row { background: rgba(28, 12, 6, 0.95); }"
    ".tree-sticky-row:hover { background: rgba(190, 110, 50, 0.12); }"
    ".tree-arrow { color: #6a3820; }"
    ".tree-arrow-open { color: #d08040; }"
    ".tree-name { color: #d0a070; }"
    ".tree-name-dir { color: #f0c890; }"
    /* Buffer / editor */
    ".buffer-pane, .buffer-pane-active { background: transparent; }"
    ".buffer-tabs-row { background: rgba(16, 7, 3, 0.87); }"
    ".buffer-tab { background: transparent; corner-radius: 0px; }"
    ".buffer-tab:hover { background: rgba(180, 100, 40, 0.12); }"
    ".buffer-tab-active { background: rgba(76, 36, 16, 0.92); }"
    ".buffer-tab-text { color: #7a4020; }"
    ".buffer-tab-text-active { color: #f4ddb8; }"
    ".buffer-tab-close:hover { background: rgba(220, 80, 40, 0.20); }"
    ".buffer-body { background: rgba(14, 6, 2, 0.93); }"
    ".buffer-gutter-col { background: rgba(18, 8, 4, 0.86); }"
    ".buffer-gutter-line { color: #4a2810; }"
    ".buffer-line, .buffer-body-text, .hl-plain { color: #e8d4b8; }"
    ".buffer-selection { background: rgba(100, 48, 18, 0.90); }"
    ".buffer-caret { background: #e08040; }"
    ".buffer-scrollbar { background: rgba(18, 8, 4, 0.30); }"
    ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(160, 90, 40, 0.38); corner-radius: 0px;"
    "}"
    ".buffer-scrollbar-thumb:hover, .buffer-scrollbar-thumb-active,"
    ".buffer-hscrollbar-thumb:hover, .buffer-hscrollbar-thumb-active {"
    "  background: rgba(200, 120, 60, 0.56);"
    "}"
    /* Syntax */
    ".hl-keyword, .hl-macro { color: #e8a060; }"
    ".hl-comment { color: #6a4020; }"
    ".hl-string, .hl-regex { color: #a8c870; }"
    ".hl-number, .hl-constant, .hl-escape { color: #f0b860; }"
    ".hl-function, .hl-constructor { color: #e0c880; }"
    ".hl-type, .hl-namespace { color: #78d0b0; }"
    ".hl-property, .hl-attribute, .hl-parameter { color: #d8b888; }"
    ".hl-operator, .hl-label { color: #c09060; }"
    ".hl-tag { color: #e86050; }"
    /* Welcome */
    ".welcome-pane { background: rgba(14, 6, 2, 0.91); }"
    ".welcome-title { color: #f0c898; }"
    ".welcome-subtitle, .welcome-section-label { color: #6a4020; }"
    ".welcome-hr { background: rgba(160, 90, 40, 0.24); }"
    ".welcome-btn { background: rgba(140, 80, 32, 0.12); color: #c0a060; }"
    ".welcome-btn-primary { background: rgba(110, 56, 20, 0.28); color: #f0d4a0; }"
    ".welcome-btn:hover, .welcome-btn-primary:hover { background: rgba(170, 100, 44, 0.26); }"
    /* Aux windows */
    ".fp-root, .search-root-window { background: rgba(16, 7, 3, 0.91); }"
    ".fp-toolbar, .fp-footer, .fp-colhdr, .search-header, .search-footer {"
    "  background: rgba(22, 10, 5, 0.93);"
    "}"
    ".fp-list, .search-results { background: rgba(14, 6, 2, 0.89); }"
    ".fp-row:hover, .search-result:hover { background: rgba(180, 100, 40, 0.10); }"
    ".fp-row-selected, .search-result-selected { background: rgba(76, 36, 14, 0.90); }"
    ".fp-new-folder-input, .search-input { background: rgba(14, 6, 2, 0.89); corner-radius: 0px; color: #e8d4b8; }"
    ".search-result-line { color: #d08040; }"
    /* Plugin/Settings windows */
    ".pm-root, .sw-root { background: rgba(16, 7, 3, 0.91); }"
    ".pm-left, .sw-left { background: rgba(20, 9, 4, 0.90); }"
    ".pm-item:hover, .sw-tab-btn:hover { background: rgba(180, 100, 40, 0.10); }"
    ".pm-item-selected, .sw-tab-btn-active { background: rgba(76, 36, 14, 0.82); }"
    ".pm-right, .sw-right { background: rgba(14, 6, 2, 0.60); }"
    ".sw-hr, .pm-hr { background: #4a2810; }"
    ".pm-btn, .pm-btn-enable, .pm-btn-disable, .sw-effect-btn { background: rgba(150, 80, 30, 0.14); corner-radius: 0px; }"
    ".pm-btn:hover, .sw-effect-btn:hover { background: rgba(180, 100, 44, 0.22); }"
    ".sw-effect-btn-active { background: rgba(100, 48, 16, 0.36); }"
    /* Command flow popup */
    ".cf-panel { background: rgba(14, 6, 2, 0.96); corner-radius: 0px; }"
    ".cf-row-key { background: rgba(170, 96, 40, 0.16); corner-radius: 0px; }"
    ".cf-row-key-text { color: #e08040; }"
    /* Terminal */
    ".term-panel, .term-viewport, .term-filler { background: rgba(14, 6, 2, 0.97); }"
    ".term-header { background: rgba(20, 9, 4, 0.97); }"
    ".term-tab { background: rgba(20, 9, 4, 0.97); color: #6a4020; }"
    ".term-tab-active { background: rgba(14, 6, 2, 0.97); color: #e8c080; }"
    ".term-cursor-focused { background: #e08040; color: #0c0400; }";

/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */
/* ------------------------------------------------------------------ */

static const struct {
    const char *id;
    const char *name;
    const char *css;
} k_themes[] = {
    { "com.sol.theme.ocean",     "Deep Ocean",  k_ocean_css     },
    { "com.sol.theme.amethyst",  "Amethyst",    k_amethyst_css  },
    { "com.sol.theme.graphite",  "Graphite",    k_graphite_css  },
    { "com.sol.theme.ember",     "Ember Glass", k_ember_css     },
};

/*
 * Register all four theme overrides, each extending the Glass baseline.
 *
 * ctx  Plugin context for registration tracking.
 */
static bool themes_on_load(SolPluginCtx *ctx)
{
    for (size_t i = 0u; i < sizeof(k_themes) / sizeof(k_themes[0]); ++i) {
        sol_plugin_register_theme(ctx, &(SolThemeDesc){
            .id      = k_themes[i].id,
            .name    = k_themes[i].name,
            .base_id = "com.sol.theme.glass",
            .css     = k_themes[i].css,
        });
    }
    return true;
}

static void themes_on_unload(SolPluginCtx *ctx)
{
    (void)ctx;
}

bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION) return false;
    *out_api = (SolPluginAPI){
        .api_version  = SOL_PLUGIN_API_VERSION,
        .id           = "com.sol.themes",
        .display_name = "Sol Themes",
        .version      = "1.0.0",
        .after        = { NULL },
        .on_load      = themes_on_load,
        .on_unload    = themes_on_unload,
    };
    return true;
}
