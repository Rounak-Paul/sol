# Buffer extents stop short of the pane (2026-09-27)

Symptom: open a terminal and dock it right (`L t v`); the buffer's rows end well above the
pane's bottom edge even though the full height is free.

## Root cause (reproduced with stderr diagnostics, since removed)
The text view sizes its row count from `sol_ui_buffer_area_rect_internal` (workspace.c), which
derives the extent from the cached `ui->window_w/h`. That cache is set to 1080x720 at UI creation
and only updated by `on_window_resize` in input_router.c. There is ONE shared router, bound to the
active project only (`sol_project_activate`, main.c). So any project created, or backgrounded,
while the window was resized/maximized kept stale dimensions.
Measured: maximized window 1512x949, new project -> calc pane h 625 vs real 862.6; 28 rows
rendered into an 862px pane. Moving the terminal right exposes the gap most clearly.

## Fixes
1. `sol_project_activate` (main.c): read the size from the outgoing active UI (the only one
   receiving resize events) and push it into the incoming UI via
   `sol_ui_system_on_window_resize` (also bumps `sig_window_rev` -> rebuild).
2. `sol_ui_buffer_area_rect_internal`: subtracted `panel_margin * 2` vertically, but
   `.workspace-main-content` pads top/left/right only (bottom 0). Now subtracts one margin.
   Computed rect now matches the laid-out pane exactly for none/BOTTOM/RIGHT (verified live).

Consumers that inherit the corrected rect: text_view row count, input_router settle/scroll/
terminal hit-test (`terminal_cell_at_point` inverts the ratio from this rect), main.c line ~372.
