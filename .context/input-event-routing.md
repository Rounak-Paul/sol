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
