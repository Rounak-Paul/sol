#ifndef SOL_UI_CONSTANTS_H
#define SOL_UI_CONSTANTS_H

/* Shared UI typography baseline used at boot and in stylesheet text rules. */
#define SOL_UI_BOOT_FONT_SIZE_PX 12
#define SOL_UI_BOOT_FONT_SIZE_PX_FLOAT 12.0f
#define SOL_UI_BOOT_FONT_SIZE_PX_CSS "12px"

/* Floating workspace composition geometry in logical CSS pixels. */
#define SOL_UI_PANEL_MARGIN_PX 8.0f
#define SOL_UI_PANEL_GAP_PX 8.0f
#define SOL_UI_PANEL_RADIUS_PX 8.0f
#define SOL_UI_PANEL_MARGIN_PX_CSS "8px"
#define SOL_UI_PANEL_RADIUS_PX_CSS "8px"

/* Rounding scale shared across the whole UI (see the "Floating rounded
   glass composition" block in style.h and sol_settings_build_appearance_css):
     control  — tabs, badges, buttons, inputs, popup rows, scrollbar thumbs
     panel    — floating panels, popups/menus, cards
     pill     — full-height rounded strips (status bar) */
#define SOL_UI_CONTROL_RADIUS_PX 4.0f
#define SOL_UI_CONTROL_RADIUS_PX_CSS "4px"
#define SOL_UI_PILL_RADIUS_PX_CSS "10px"

/* Surface elevation opacity scale shared by every window's Glass-theme
   background (style.h "Minimal glass theme overrides") and mirrored by
   each curated theme's generated CSS (sol-plugin-themes' build_theme_css).
   Applies uniformly across the main workspace panels AND every auxiliary
   dialog window (Settings, Plugin Manager, File Picker, Search) so no
   surface reads as a visually distinct subsystem:
     chrome — window/panel roots: main floating panels, dialog window
              roots, popups, status bar, command-flow overlay
     raised — headers/toolbars/strips sitting above a chrome floor:
              tab rows, list/section headers, search bars
     well   — recessed input fields and scroll wells: text inputs,
              list/result containers */
#define SOL_UI_SURFACE_CHROME_ALPHA_CSS "0.86"
#define SOL_UI_SURFACE_RAISED_ALPHA_CSS "0.90"
#define SOL_UI_SURFACE_WELL_ALPHA_CSS   "0.86"

/* Focused-panel indicator: a thin inset bright-orange border applied to
   whichever top-level panel currently owns keyboard focus. See
   SolUIFocusedPanel / sol_ui_system_set_focused_panel. Uses a real
   border, not a shadow-based glow: this engine's shadow SDF resolves to
   full opacity for every fragment inside the shape (the Gaussian
   falloff only applies outside it), so a centered "glow" shadow paints
   as a solid rectangle rather than a soft ring. */
#define SOL_UI_FOCUS_BORDER_WIDTH_PX_CSS "1px"
#define SOL_UI_FOCUS_BORDER_COLOR_CSS "rgba(255, 140, 0, 0.95)"

#endif
