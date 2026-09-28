# In-buffer find (L b f → buffer.find) — 2026-09-28

## Behaviour
- `buffer.find` (default chord leader+b f, label "Find in buffer") is registered
  in code by `sol_register_search_command_defaults` (main.c), so it works with a
  pre-existing bindings.conf; also in the template (`bind L b f buffer.find`),
  the Buffer description block, and Edit > Find in Buffer (item_order 30).
- The query is typed into the status bar (`[F] find: <query>  3/17`,
  `no matches`, `type to search`, `N/262144+` when capped).
- Every query change re-scans; the match nearest the caret at open is previewed
  as a selection. Up/Down cycle (wrap), Enter lands the caret at the match
  start (no selection), Esc restores caret/selection/scroll, Backspace pops one
  UTF-8 codepoint. Ctrl/Alt/Super chords and leader chords pass through.
- Off-screen jumps are centered (`post_edit_settle(..., center=true)` in
  input_router.c); normal editing still scrolls minimally.
- Matching: ASCII case-insensitive, non-ASCII bytes exact, non-overlapping.

## Code map
- Core: `sol_text_buffer_find_all` (streams rope through a 64 KiB window
  overlapping by needle_len-1; needle ≤ `SOL_TEXT_BUFFER_FIND_MAX_NEEDLE` 1024;
  returns total, stores ≤ cap) and `sol_text_buffer_select_range`.
- UI: `sol/src/ui/buffer_find.c`; state `SolUIBufferFind` in `SolUISystem`
  (sol_ui_internal.h). Public API in sol_ui_system.h: open/close/active/key/
  char/matches. `SolUIFindInput` IGNORED/CONSUMED/MOVED (router settles on
  MOVED). `SolUIFindClose` LAND/CANCEL/RELEASE.
- Observers: subscribes to TEXT_EDITED (rescan), BUFFER_FOCUSED (other buffer →
  release), BUFFER_CLOSED (target → release) only while active; bus from
  `sol_buffer_event_bus(ui->buffers)`.
- Session ends (RELEASE) on: any primary-window mouse down (router), focus moving
  off the buffer panel (`sol_ui_system_set_focused_panel`), project deactivate
  (`sol_ui_system_set_active(false)`); `sol_ui_buffer_find_shutdown` in destroy.
- Rendering: text_view.c draws `.buffer-find-match` (others, behind selection)
  and `.buffer-find-match-current` (above selection). Status bar:
  `sol_ui_buffer_find_render_status` (strings live in the session — Causality
  keeps ca_text pointers). Themes: plugin.c derives both from `theme->warning`.
- Test stub: tests/stubs/sol_ui_bump_stub.c stubs the status renderer.

## Verification
- Unit: sol_text_buffer_tests find_all (case, edges, window straddle), select_range.
- Live (2026-09-28): temporary router-feed hook + VK screenshot layer drove
  ctrl-tap, b, f, "92%", Down, Enter, then "_css", Down, Esc through the real
  on_key/on_char path; status prompt, highlights, landing, Esc restore and
  centering all confirmed in frames. Hook removed.

## Related: Lff vs "92%"
Lff (`find.files`) fuzzy-matches file *paths* only. Lfg (`find.grep`) finds
"92%" in sol/src/ui/style.h, but the index skips every dot-directory
(`search_is_ignored_dir` / walk skips names starting with '.'), so `.context/`
content is never searched.
