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

## Terminal focus leak into plugin-owned Ca_TextInput widgets (2026-09-08)

User-reported bug: typing a commit message in the git panel's commit-message box also typed
into the terminal last used. Root cause: `Ca_TextInput` (Causality's native text-input widget,
used by `ca_input()` — e.g. the git plugin's commit-message and new-branch-name boxes) has no
`on_focus` callback, only `on_change`, and nothing wires a click into it to
`sol_ui_system_set_focused_panel`. So `sol_terminal_manager_focused` stays true from a prior
terminal use, and `on_key`/`on_char` in `input_router.c` check that sticky bool FIRST — before
Causality's own widget dispatch even runs — and forward every keystroke straight to the PTY
regardless of which widget actually has real (Causality-level) keyboard focus.

Fix: any plugin/panel with a `Ca_TextInput` needs to poll `ca_input_is_focused(input)` once per
frame (in its own tick callback, since there's no on-focus event) and call
`sol_ui_system_set_focused_panel(ui, <its panel enum>)` whenever true — see
`git_panel_tick` in `plugins/sol-plugin-git/src/plugin.c`, which does this for both
`commit_input` and `branch_input`, using `SOL_UI_FOCUSED_PANEL_TREE` (the sidebar-panel value,
since the git panel occupies the same sidebar slot as the file tree). The setter is already a
no-op once the panel is already the focused one, so calling it unconditionally every tick while
focused is cheap. **This is a general gap, not just a git-plugin bug**: any future panel that
puts a `Ca_TextInput` in front of the user and doesn't do this same poll-and-funnel will leak
terminal (or buffer) keystrokes the same way. Causality has no public "is any input focused"
query — only per-instance `ca_input_is_focused(specific_input)` — so this must be done per input,
not fixed once at the router level.

## Dual focus-system arbitration — root fix (2026-09-26)

Root cause of the whole "one key hits two targets" class (e.g. `L t t` then Enter both reaches
the PTY and re-fires the welcome/central button clicked earlier): Causality keeps its own
`win->focused_node` (set by click and Tab) and, in `ca_widget_input_pass`, runs Tab navigation
and Enter/Space button activation from the raw per-frame `key_buf` — independent of what Sol's
router already did with the same key during event dispatch. Previous fixes patched individual
buttons with `skip_keyboard_focus`; any new button reintroduced the bug.

Fix (Causality API, opt-in per window):
- `ca_window_set_app_keyboard(win, true)` — buttons never take native focus (click or Tab), Tab
  navigation and Enter/Space activation are disabled; text inputs still focus by click /
  `ca_input_focus` and get keys. Enabling evicts a focused button. Flag: `Ca_Window.app_keyboard`.
- `ca_window_clear_focus(win)` — drops native focus + reapplies :focus CSS on the old chain.
- Regression test: `test_app_keyboard_blocks_button_activation` in
  `vendors/causality/causality/tests/ca_input_capture_tests.c` (binary `bin/causality_input_capture_tests`).

Sol wiring:
- `main.c`: primary `host.window` enables app keyboard right after creation. Secondary windows
  (search, settings, file picker, ssh, plugin manager) stay native so their Tab/Enter UX works.
- `workspace.c: sol_ui_system_set_focused_panel` clears native focus when the target is TERMINAL
  or BUFFER (before the same-panel early return), so a focused text input releases the keyboard.
  TREE deliberately does not clear: the git plugin re-asserts TREE every tick while its input is focused.
- `input_router.c: native_ui_owns_keyboard()` (wraps `ca_window_input_capture`) — while a Causality
  text input/modal owns the keyboard, `on_key` skips the terminal path and buffer edits and `on_char`
  drops the char. Keymap/leader processing still runs. This makes the git plugin's per-input poll
  only a panel-highlight concern, no longer required to prevent keystroke leaks.

Build note: after an Xcode/clang update, delete `build/**/cmake_pch*.pch` if the build fails with
"PCH file built from a different branch".
