#ifndef SOL_UI_STYLE_H
#define SOL_UI_STYLE_H

/* Window presentation defaults. */
#define SOL_UI_WINDOW_TITLE "Sol"
#define SOL_UI_WINDOW_WIDTH 1080
#define SOL_UI_WINDOW_HEIGHT 720

/* Split bar presentation defaults. */
#define SOL_UI_SPLIT_BAR_SIZE 1.0f
#define SOL_UI_SPLIT_BAR_COLOR ca_color(0.22f, 0.24f, 0.30f, 1.0f)
#define SOL_UI_SPLIT_BAR_HOVER_COLOR ca_color(0.34f, 0.37f, 0.47f, 1.0f)

/*
 * Main window CSS for Sol.
 * Keep style edits in this file to iterate quickly without touching logic.
 */
static const char *SOL_UI_MAIN_WINDOW_CSS =
    ".app-root {"
    "  background: #0c0e13;"
    "  padding: 0px;"
    "  gap: 0px;"
    "  width: 100%;"
    "  height: 100%;"
    "  min-width: 0px;"
    "  min-height: 0px;"
    "  overflow: hidden;"
    "}"
    ".workspace-host {"
    "  background: #11151f;"
    "  border-radius: 0px;"
    "  padding: 0px;"
    "  width: 100%;"
    "  height: 100%;"
    "  min-width: 0px;"
    "  min-height: 0px;"
    "  flex-grow: 1;"
    "  flex-shrink: 1;"
    "  overflow: hidden;"
    "}"
    ".buffer-pane {"
    "  background: #171c2a;"
    "  border-radius: 0px;"
    "  padding: 0px;"
    "  gap: 0px;"
    "  width: 100%;"
    "  height: 100%;"
    "  min-width: 0px;"
    "  min-height: 0px;"
    "  flex-grow: 1;"
    "  flex-shrink: 1;"
    "  overflow: hidden;"
    "}"
    ".buffer-pane-active {"
    "  background: #20283a;"
    "}"
    ".buffer-body {"
    "  background: #0f1320;"
    "  border-radius: 0px;"
    "  padding: 8px;"
    "  width: 100%;"
    "  height: 100%;"
    "  min-width: 0px;"
    "  min-height: 0px;"
    "  flex-grow: 1;"
    "  flex-shrink: 1;"
    "  overflow: hidden;"
    "}"
    ".buffer-body-text {"
    "  color: #d7deef;"
    "  font-size: 14px;"
    "  text-wrap: wrap;"
    "  overflow: hidden;"
    "}";

#endif
