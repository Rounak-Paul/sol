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

Reader thread reads from master fd (blocking `read`/`ReadFile`) into a 512KB circular ring buffer.
After each deposit it rate-limits wakes via `wake_pending` atomic: only calls `ca_instance_wake()`
if the previous wake has been consumed (drain clears `wake_pending` at the start of each call).
This bounds wake rate to ~one per display frame even under flood output (yes, cmatrix).

Main thread drains in `sol_ui_on_frame` via `sol_terminal_manager_drain()`:
- Clears `wake_pending` first so reader can schedule the next wake immediately
- Consumes at most `SOL_TERM_DRAIN_BYTES_PER_FRAME` (64KB) per call
- If ring has more bytes, sets `wake_pending=true` and calls `ca_instance_wake()` itself to schedule next frame drain
- VT parser processes drained bytes → bumps `sig_terminal_rev` if dirty

This caps per-frame VT-parse CPU at ~64KB × parse overhead, keeping the UI responsive at ≤60 fps
even when a process floods the terminal at MB/s rates.

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

Bindings are also written to `~/.sol/bindings.conf` on first launch via `SOL_DEFAULT_BINDINGS_CONF`.

| Action | Binding | Description |
|--------|---------|-------------|
| `terminal.toggle` | L t t | Show/hide terminal + focus toggle |
| `terminal.position.bottom` | L t h | Dock at bottom; retains focus if visible |
| `terminal.position.right` | L t v | Dock on right; retains focus if visible |
| `terminal.kill` | L t x | Kill active terminal tab |
| `terminal.tab.new` | L t c | Open new terminal tab + focus it |
| `terminal.tab.next` | L t n | Switch to next tab + show + focus |
| `terminal.tab.prev` | L t p | Switch to prev tab + show + focus |

The `terminal.toggle` action:
1. If no tabs exist: creates one first
2. If not visible: show + focus
3. If visible but not focused: focus
4. If visible and focused: defocus (panel stays visible)

`terminal.tab.next` / `terminal.tab.prev`: always show + focus terminal after tab switch
so the user can immediately type without pressing ESC first.

`terminal.position.bottom` / `terminal.position.right`: only set focus if the terminal
is already visible (avoids surprise focus steal when configuring layout while editing).

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

## Crash Fix — alt_screen resize (2026-06-12)

`SolTerminal` has `alt_screen_rows` (int) tracking how many rows `alt_screen` was
allocated for.  Before this fix, `sol_terminal_destroy` looped `for (r < term->rows)`
on `alt_screen`, but `term->rows` grows on resize while `alt_screen` kept its original
allocation size — `term_line_free` on entries beyond that size freed garbage pointers.

Changes in `sol_terminal.c`:
- `struct SolTerminal`: added `int alt_screen_rows` field.
- Allocation site (`vt_set_dec_mode` case 47): sets `term->alt_screen_rows = term->rows`
  after `calloc`.
- `sol_terminal_destroy`: uses `alt_screen_rows` (not `term->rows`) for the free loop.
- `sol_terminal_resize`: after resizing `screen`, syncs `alt_screen` to new dims:
  - Grow (new rows > alt_screen_rows): `realloc`, zero+alloc new entries, update
    `alt_screen_rows`.
  - Column resize: `realloc` each row's `cells` array.
  - Shrink (new rows < alt_screen_rows): free excess rows, update `alt_screen_rows`.
  On alloc failure during grow, size is kept at old value (safer to clip than corrupt).

Child process isolation: `forkpty` scopes child signals (SIGSEGV etc.) to the child
process only.  The reader thread exits cleanly on EIO when the PTY slave closes.
`sol_terminal_manager_drain` reaps dead children via `waitpid(WNOHANG)` each frame.

## Performance (large terminals / cmatrix)

Three bottlenecks fixed (2026-06-12):

1. **Drain reads only 4 KB/frame** — drain local buffer enlarged to `SOL_TERM_OUTPUT_RING_SIZE`
   so the entire ring is consumed in one call, eliminating multi-frame backlog and perceived lag.

2. **Ring buffer 64 KB** — raised to 512 KB (`SOL_TERM_OUTPUT_RING_SIZE = 524288`) and reader
   thread read buffer from 4 KB to 32 KB, handling large bursty output without dropping bytes.

3. **Full terminal rebuild every vsync frame when focused** — `on_frame` was unconditionally
   bumping `sig_terminal_rev` for cursor blink.  Replaced with a 530 ms timer: blink phase
   stored in `ui->term_cursor_blink_on` (SolUISystem), toggled in `on_frame`, read by
   `sol_ui_render_terminal_panel`.  Idle-but-focused terminals now rebuild at ~2 fps instead
   of 60 fps.  When actual output arrives (drain returns true) the rebuild still fires
   immediately.

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
