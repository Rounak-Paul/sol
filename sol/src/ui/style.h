#ifndef SOL_UI_STYLE_H
#define SOL_UI_STYLE_H

/*
 * Main window CSS for Sol.
 * Keep style edits in this file to iterate quickly without touching logic.
 */
static const char *SOL_UI_MAIN_WINDOW_CSS =
    ".app-root {"
    "  background: #0c0e13;"
    "  padding: 16px;"
    "  gap: 10px;"
    "}"
    ".title {"
    "  color: #ecf3ff;"
    "  font-size: 30px;"
    "}"
    ".subtitle {"
    "  color: #8d99ae;"
    "  font-size: 13px;"
    "}"
    ".workspace-host {"
    "  background: #11151f;"
    "  border-radius: 10px;"
    "  padding: 8px;"
    "  height: 0;"
    "}"
    ".buffer-pane {"
    "  background: #171c2a;"
    "  border-radius: 8px;"
    "  padding: 8px;"
    "  gap: 8px;"
    "}"
    ".buffer-pane-active {"
    "  background: #20283a;"
    "}"
    ".buffer-name {"
    "  color: #9ecbff;"
    "  font-size: 13px;"
    "}"
    ".buffer-body {"
    "  background: #0f1320;"
    "  border-radius: 6px;"
    "  padding: 10px;"
    "}"
    ".buffer-body-text {"
    "  color: #d7deef;"
    "  font-size: 14px;"
    "  wrap: true;"
    "}"
    ".status-line {"
    "  color: #7ee29f;"
    "  font-size: 12px;"
    "}";

#endif
