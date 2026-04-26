// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_style.h — sol's full stylesheet, embedded as a string literal.
// Sharp aesthetic: corners are 0–2px, never more.

#ifndef SOL_STYLE_H
#define SOL_STYLE_H

static const char *const SOL_STYLESHEET =
    /* ---- Theme tokens ---- */
    ":root {"
    "  --bg:        #0e1116;"
    "  --bg-alt:    #161b22;"
    "  --bg-active: #1f2630;"
    "  --fg:        #d6d6d6;"
    "  --fg-dim:    #8b95a3;"
    "  --accent:    #79b8ff;"
    "  --warn:      #f5c542;"
    "  --line:      #2a313c;"
    "}"

    /* ---- Layout root ---- */
    ".root {"
    "  background: #0e1116;"
    "  color:      #d6d6d6;"
    "  flex-direction: column;"
    "  width:  100%;"
    "  height: 100%;"
    "}"

    /* ---- Tab strip ---- */
    ".tabs {"
    "  background: #161b22;"
    "  padding: 0 6px;"
    "  gap: 0;"
    "  flex-direction: row;"
    "  height: 26px;"
    "  align-items: center;"
    "}"
    ".tab {"
    "  color: #8b95a3;"
    "  padding: 4px 12px;"
    "  background: #161b22;"
    "  border-radius: 0;"
    "  font-size: 12px;"
    "}"
    ".tab.active {"
    "  color:      #d6d6d6;"
    "  background: #0e1116;"
    "  border-radius: 0;"
    "}"

    /* ---- Editor area ---- */
    ".editor-pane {"
    "  background: #0e1116;"
    "  flex-direction: column;"
    "  flex-grow: 1;"
    "  overflow: hidden;"
    "  padding: 4px 0;"
    "}"
    ".editor-pane.empty {"
    "  align-items: center;"
    "  justify-content: center;"
    "}"
    ".empty-hint { color: #8b95a3; font-size: 13px; }"
    ".editor-line {"
    "  flex-direction: row;"
    "  gap: 8px;"
    "  padding: 0 8px;"
    "  height: 18px;"
    "}"
    ".editor-line.current { background: #161b22; }"
    ".line-number { color: #4a5360; font-size: 12px; width: 36px; }"
    ".line-text   { color: #d6d6d6; font-size: 12px; }"
    ".cursor-cell {"
    "  background: #79b8ff;"
    "  color: #0e1116;"
    "  font-size: 12px;"
    "  border-radius: 0;"
    "}"

    /* ---- Status bar ---- */
    ".status-bar {"
    "  background: #161b22;"
    "  color: #d6d6d6;"
    "  flex-direction: row;"
    "  height: 22px;"
    "  padding: 0 8px;"
    "  align-items: center;"
    "}"
    ".status-text  { color: #d6d6d6; font-size: 12px; }"
    ".status-chord { color: #f5c542; font-size: 12px; }"

    /* ---- Which-key flow popup ---- */
    ".flow-popup {"
    "  background: #161b22;"
    "  border-radius: 2px;"
    "  padding: 8px 0;"
    "  flex-direction: column;"
    "  gap: 0;"
    "}"
    ".flow-title {"
    "  color: #79b8ff;"
    "  font-size: 12px;"
    "  padding: 0 12px 6px 12px;"
    "}"
    ".flow-row {"
    "  color: #d6d6d6;"
    "  font-size: 12px;"
    "  padding: 2px 12px;"
    "  height: 18px;"
    "}"
    ;

#endif
