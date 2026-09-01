Task context: isolate input events by active Sol UI region/window.

Observed leak:
- Causality posts key/char/scroll events per `Ca_Window`, but Sol's global input router handled them as if every event belonged to the primary editor window.
- `on_char` inserted into the active text buffer whenever a buffer existed, so typing in secondary windows such as search could also mutate the editor.
- `on_mouse_scroll` fell back to the active buffer when the mouse was outside the buffer split-tree, so scrolling the explorer/sidebar could scroll the editor.
- `on_window_resize` updated primary workspace size for any window resize.

Fix design:
- Treat Sol text-buffer editing focus as explicit router state.
- Only events from `sol_ui_system_primary_window(ui)` are allowed to affect editor buffers or command-flow UI.
- Primary-window mouse press inside a buffer leaf activates buffer editing; pressing outside the buffer area clears it.
- Buffer keyboard edits and printable character insertion require active buffer-edit focus.
- Buffer wheel scrolling requires the mouse to be inside the buffer split-tree and over a concrete leaf; never fall back to active buffer for wheel routing.
- Primary workspace resize state updates only for resize events from the primary window.
- Router starts with buffer editing active when a buffer exists so launch behavior remains editor-like. Non-primary events and primary clicks outside the buffer clear that state; clicking a buffer leaf restores it.

## Terminal focus stickiness (2026-09-01)

`SolTerminalManager` keyboard focus is a separate sticky bool (`sol_terminal_manager_focused`),
checked FIRST in `on_key`/`on_char` before any other routing — so it can swallow every
keystroke even when the visible focused panel (`SolUIFocusedPanel` on `SolUISystem`) has moved
elsewhere, if nothing explicitly clears it.

Fix: `sol_ui_system_set_focused_panel` (workspace.c) is the single funnel every "focus moved to
panel X" call site uses (buffer tab, buffer pane click, file tree row). It now clears terminal
manager focus whenever the target panel isn't `SOL_UI_FOCUSED_PANEL_TERMINAL`. Also: clicking
directly in the raw editor text area (`on_mouse_button` in input_router.c, the
`point_in_active_buffer_leaf` branch) bypassed this funnel entirely — it only set the router's
own `buffer_input_active`, never called `sol_ui_system_set_focused_panel`. Fixed to call it too.

## Programmatic focus restoration (`L b b` / buffer.focus.previous)

`buffer.focus.previous` (main.c) originally always called `sol_buffer_focus_previous_buffer`
(tab-swap to the alternate buffer) regardless of which panel had keyboard focus — so invoking
it while focused in the terminal/tree both failed to restore keyboard focus to the buffer AND
fired an unwanted tab-cycle side effect. There was no other binding to return focus to the
buffer panel once lost.

Fixed: the action now checks `sol_ui_system_focused_panel(ui) != SOL_UI_FOCUSED_PANEL_BUFFER`
first — if so, it's a pure "bring focus back" and only calls `sol_ui_system_set_focused_panel`
(clears terminal focus via the funnel above) + a new `sol_input_router_set_buffer_input_active`
setter, no tab-cycle. Only performs the alternate-buffer tab-swap when the buffer panel already
has focus, matching the original semantics.

`sol_input_router_set_buffer_input_active(router, bool)` is a new public router API — the
router's `buffer_input_active` field (gates whether KEY_DOWN reaches `handle_text_buffer_key`)
was previously only ever set from observed mouse clicks inside `input_router.c` itself; there
was no way for a command action to programmatically restore it. This is the general pattern for
any future "focus panel X via command/keybinding" action that targets the buffer.
