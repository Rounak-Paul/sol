# Terminal Architecture

## Files

| File | Role |
|------|------|
| `sol/include/sol_terminal.h` | Public API: cell types, manager, per-terminal ops, color conversion |
| `sol/src/core/sol_terminal.c` | VT state machine, cell grid, PTY (forkpty / ConPTY), reader thread |
| `sol/src/ui/terminal_panel.c` | Causality rendering: tab strip, row-by-run cell grid, click-to-focus |

## Key Types

- `SolTermCell` — one terminal grid cell (codepoint, fg/bg color, attrs bitmask)
- `SolTermLine` — one row of cells + dirty flag
- `SolTermColor` — color with mode: DEFAULT, INDEXED (256), RGB (true color)
- `SolTerminal` — single session (VT state + cell grid + PTY + reader thread)
- `SolTerminalManager` — multi-tab manager, visibility, focus, layout position

## VT Parser

Paul Williams state machine implemented as switch-per-state dispatch in `vt_process_byte()`.
UTF-8 multi-byte sequences accumulated in `VtUtf8` struct before calling `term_put_char()`.

States: GROUND, ESCAPE, ESCAPE_INT, CSI_ENTRY, CSI_PARAM, CSI_INT, CSI_IGNORE,
        DCS_ENTRY, DCS_PARAM, DCS_INT, DCS_PASSTHROUGH, DCS_IGNORE,
        OSC_STRING, SOS_PM_APC

Handled sequences: cursor movement (A-H,f,G,d), erase (J,K,X), insert/delete (L,M,P,@),
scroll (S,T), SGR (m), modes (h/l + DEC private ?), DECSTBM (r), save/restore (s/u, ESC 7/8),
OSC title (0/1/2), C0 controls, alt screen (?1049h/l), DA, CPR.

## Cell Grid Model

- `screen[rows][cols]` — current viewport (always in memory)
- `scrollback[]` — circular buffer of lines that scrolled off the top (cap: 5000)
- `view_scroll` — 0 = viewport at bottom; k = k lines of scrollback visible above
- Alt screen: separate `alt_screen` grid allocated lazily; saved/restored on ?1049 toggle

When a line scrolls off the scroll-region top: copied into scrollback ring (main screen only),
viewport lines shift up.

## PTY Backend

**Unix (macOS / Linux):** `forkpty()` (macOS: `<util.h>`, Linux: `<pty.h>`).
Child execs `$SHELL`. Master fd kept in parent. `TIOCSWINSZ` ioctl for resize.
**Windows:** ConPTY via `CreatePseudoConsole` (Win10 1809+). Dynamic loading from `kernel32.dll`.

## Threading

Reader thread reads from master fd (blocking `read`/`ReadFile`) into a 64KB circular ring buffer.
After each deposit it calls `ca_instance_wake()`. Main thread drains in `sol_ui_on_frame` via
`sol_terminal_manager_drain(ui->terminal_mgr)` → VT parser processes bytes → bumps
`sig_terminal_rev` if dirty.

## Workspace Integration

`SolUISystem` (in `sol_ui_internal.h`) gained:
- `terminal_mgr`: `SolTerminalManager*` (set via `sol_ui_system_set_terminal_manager`)
- `sig_terminal_rev`: `Ca_Signal*` (bumped on drain output, focus change, or terminal notify)

Public API in `sol_ui_system.h`:
- `sol_ui_system_set_terminal_manager(ui, mgr)`
- `sol_ui_system_terminal_manager(ui)` → `SolTerminalManager*`
- `sol_ui_system_terminal_notify(ui)` — bumps `sig_terminal_rev`

`sol_ui_workspace_content_builder` subscribes to `sig_terminal_rev` and calls
`sol_ui_render_buffer_and_terminal(ui, term_visible)` which:
- If terminal visible: emits `ca_split_begin` (CA_VERTICAL for BOTTOM, CA_HORIZONTAL for RIGHT)
  with buffer pane first (ratio = 1 - term_ratio), terminal pane second.
- If terminal not visible: emits buffer tabs + workspace tree directly.

`sol_ui_on_frame` calls `sol_terminal_manager_drain` each frame and bumps `sig_terminal_rev`
if any output was processed. Also bumps + wakes if terminal is focused (cursor blink).

`sol_ui_on_terminal_resize(ratio)` callback updates `terminal_mgr` ratio as
`1.0 - ratio` (since `ratio` is the fraction of the first/buffer pane).

## Input Routing

`on_key` in `input_router.c` checks `sol_ui_system_terminal_manager(r->ui)` first:
- If terminal focused + ESC → defocus (`set_focused(false)`) + notify, return
- If terminal focused + other key → `sol_terminal_send_key(term, key, mods)`, return
- Suppresses next CHAR event after a KEY_DOWN in terminal mode

`on_char` in `input_router.c` checks terminal focus:
- If focused + printable codepoint → UTF-8 encode, `sol_terminal_send_text`, return

`on_mouse_scroll` routes vertical scroll to `sol_terminal_set_view_scroll` when terminal focused.

Focus is set by clicking the terminal viewport (Ca_Button click handler) or via `L t t` command.
Defocus happens on ESC keypress.

## Command Flows (registered in main.c via `sol_register_terminal_command_defaults`)

| Action | Binding | Description |
|--------|---------|-------------|
| `terminal.toggle` | L t T | Show/hide terminal + focus toggle |
| `terminal.position.bottom` | L t H | Move terminal to horizontal bottom |
| `terminal.position.right` | L t V | Move terminal to vertical right |
| `terminal.kill` | L t X | Kill active terminal tab |
| `terminal.tab.new` | L t C | Open new terminal tab |
| `terminal.tab.next` | L t N | Switch to next tab |
| `terminal.tab.prev` | L t P | Switch to previous tab |

The `terminal.toggle` action:
1. If no tabs exist: creates one first
2. If not visible: show + focus
3. If visible but not focused: focus
4. If visible and focused: defocus (panel stays visible)

## Key Encoding

Arrow keys: `\033[A/B/C/D` (normal) or `\033OA/OB/OC/OD` (app cursor key mode).
Modified keys: `\033[1;NX` where N = modifier bitmask (shift=2, alt=3, ctrl=5...).
Ctrl+letter: `key & 0x1F` (e.g. Ctrl+C = 0x03).
Special: BS=0x7F, Enter=\r, Tab=\t, Shift+Tab=`\033[Z`, Delete=`\033[3~`,
Home=`\033[H`, End=`\033[F`, PageUp=`\033[5~`, PageDown=`\033[6~`.

## Rendering (terminal_panel.c)

`sol_ui_render_terminal_panel(ui)` emits:
- `term-header` div: per-tab `ca_btn_begin` buttons (CSS: `term-tab` / `term-tab-active`)
- `term-viewport` Ca_Button: click → focus; wraps per-row rendering
  - Per row: `term-line` horizontal div
    - Per same-attr run: `ca_div_begin(background=run_bg) + ca_text(color=run_fg, style=...)`
    - Cursor cell: `term-cursor-focused` (solid inverted block) or `term-cursor-unfocused` (border)
    - `background=0u` for default background (transparent, no overdraw)

The viewport fills the panel below the header via CSS `flex-grow: 1` and `flex-shrink: 1`.
This requires Causality to implement flex-shrink (see causality/src/ui/layout.c) so that
when the viewport's content_size (rows + filler) exceeds the available height, it shrinks
back to exactly `pane_h - header_h` instead of overflowing. The header has `flex-shrink: 0`
so it never shrinks.

After all rows a `.term-filler` div (flex-grow:1) absorbs the fractional pixel remainder
that integer row-count truncation leaves; it's clipped by the viewport's `overflow:hidden`.

Row/col calculation in `sol_ui_system_pre_tick` reads `usable_h`/`usable_w` directly from
the `term_viewport_host` Ca_Button's inner layout dimensions (`ca_btn_get_layout_inner_size`).
This avoids a stale-CSS mismatch during ui_scale slider drag.  Cell size:
- Height: `TERM_CELL_H_PX = 16.0f` — matches `ca_text()`'s explicit default height
  (`lbl->node->desc.height = s(16.0f)` in widget.c).  The generic leaf fallback of 20px
  is only used for truly empty nodes with no desc.height set; ca_text always sets it.
- Width:  `TERM_CELL_W_PX = 8.0f`  — matches monospace font advance at 13 px.

CSS classes added to `style.h`: `.term-panel`, `.term-header`, `.term-tab`, `.term-tab-active`,
`.term-viewport`, `.term-line`, `.term-cell`, `.term-cell-bold`, `.term-cell-italic`,
`.term-cell-bold-italic`, `.term-cursor-focused`, `.term-cursor-unfocused`, `.term-filler`

Color: `sol_term_color_to_rgba(color, is_fg)` converts SolTermColor → packed RGBA uint32.
ANSI 16-color palette defined as `k_ansi16[]` in `sol_terminal.c`.

## Build

CMake uses `file(GLOB_RECURSE)` for src/*.c so no CMakeLists changes needed.
All new files in `sol/src/` are automatically picked up.

## Lifecycle

`main.c`:
1. `sol_terminal_manager_create(instance)` → `app.terminal_mgr`
2. `sol_ui_system_set_terminal_manager(app.ui, app.terminal_mgr)`
3. `sol_register_terminal_command_defaults(app.ui)` registers all 7 command flows
4. On exit: `sol_ui_system_set_terminal_manager(app.ui, NULL)` then
   `sol_terminal_manager_destroy(app.terminal_mgr)` (before UI destroy, stops PTY threads)
