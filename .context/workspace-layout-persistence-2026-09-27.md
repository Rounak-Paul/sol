# Workspace layout persistence (2026-09-27)

Sol remembers the workspace arrangement across launches and new projects.

## Persisted (`$HOME/.sol/layout`, "key value" lines)
| key | source | range |
|---|---|---|
| tree_ratio | sidebar splitter (`ui->tree_panel_ratio`) | 0.10–0.50, def 0.20 |
| terminal_position | bottom / right / float (L t b/r/f) | def bottom |
| terminal_ratio | buffer/terminal splitter (terminal mgr, shared by bottom+right) | 0.20–0.80, def 0.30 |
| search_ratio | search-window results/preview split (`ui->search_split_ratio`) | 0.24–0.62, def 0.36 |

Ranges live only in `sol/include/sol_layout.h` (`SOL_LAYOUT_*`); workspace.c and
search_window.c splitter min/max use them. `sol_terminal.c`'s set_ratio still
hard-codes 0.20/0.80 (can't include sol_layout.h — it includes sol_terminal.h).

Kept out of settings.json on purpose: it's state, not a preference, and would
otherwise trigger the cross-instance settings watcher reload on every drag.

## Flow (observer)
- `sol/src/core/sol_layout.c`: defaults / sanitize / load (corruption-tolerant,
  long lines skipped, NaN → default, clamped) / atomic save (tmp+sync+rename).
- UI publishes `SOL_EVENT_LAYOUT_CHANGED` (sol_event.h) via
  `sol_ui_publish_layout_changed` from: tree/terminal splitter `on_resize`,
  search split `on_resize`, `sol_ui_system_set_terminal_position` (main.c's
  terminal.position.* commands now call this). Causality fires on_resize only
  when the ratio actually changes.
- main.c `sol_on_layout_changed` snapshots `sol_ui_system_get_layout` into
  `host->layout`, sets a 0.5 s debounce deadline and
  `ca_instance_request_frame_after` so the loop wakes to save. `sol_layout_flush`
  runs each loop iteration; forced flush at shutdown.
- `sol_project_create` applies `host->layout` via `sol_ui_system_apply_layout`
  right after the terminal manager is attached (splitters seed ratio only on
  first build, so it must precede the first frame).
- Live projects keep their own arrangement; last change wins on disk and seeds
  new projects/launches. Multiple Sol processes: last writer wins, atomic.

## Not persisted
Window size/position (Causality exposes no window-size getter), sidebar/terminal
visibility (terminal would need shell spawn), nested buffer split ratios (no
buffer session restore yet).

## Validation
Full build clean (no new warnings); 21/21 CTest pass incl. new
`test_layout_round_trip` / `test_layout_tolerates_corruption` (sol_config_tests).
`sol_project_native_tests` (manual, real GPU; args = two dirs) now checks
UI → event bus → host → disk → new project seeding; passes. Live smoke launch
with a layout file under an isolated HOME ran without errors. Mouse-drag
persistence not driven interactively (no input injection here).
