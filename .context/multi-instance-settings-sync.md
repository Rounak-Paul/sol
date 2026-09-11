# Multi-Instance Settings Sync (2026-09-11)

## Problem
Sol had no singleton lock — multiple `sol` processes already launched fine,
and all core editing state (buffers, PTYs, explorer tree, undo) was already
correctly per-process. The actual multi-instance gap was `~/.sol/settings.json`:

- `sol_settings_load()` ran once at startup only (`main.c` main()).
- `sol_settings_save()` (`sol_settings.c`) did `fopen(path, "wb")` + `fprintf`
  directly on the live file — no atomicity, no lock.
- No instance watched `settings.json` for external changes.

Result: two open windows → last save silently clobbers the other's changes,
with real torn-write risk if two saves landed close together, and neither
instance would see the other's change without a restart.

## Fix
1. **`sol_settings_save`** (`sol/src/core/sol_settings.c`) now writes to a
   per-process temp file (`settings.json.tmp<pid>`) in the config dir, then
   atomically swaps it in via `sol_platform_replace_file()` (already existed
   in `sol_platform.c` — `rename()` on POSIX, `MoveFileExA` +
   `MOVEFILE_REPLACE_EXISTING` on Windows). Never partially visible, never
   torn between two concurrent writers.
2. **Second independent `SolFileWatcher`** (`app.settings_watcher` in
   `main.c`, alongside the existing explorer-root `app.watcher`) watches
   `sol_config_dir()` (`~/.sol`) for the whole process lifetime. Created/
   destroyed alongside the explorer watcher; same wake-callback pattern.
3. **`sol_drain_settings_watcher(app)`** (`main.c`, called each frame next
   to `sol_drain_file_watcher`): filters watch events to basename
   `settings.json`, reloads via `sol_settings_load`, diffs field-by-field
   against `app->settings`, and re-applies only what changed — theme
   (`sol_ui_system_set_active_theme`), appearance overlay
   (`sol_ui_system_apply_appearance`), bg effect
   (`sol_bg_effect_set_active`/`set_opacity`), UI scale
   (`ca_instance_set_scale`). The diff-then-apply is also what makes an
   instance's own save not re-trigger redundant work when its own watcher
   fires on its own write (in-memory already matches the reloaded values).

## Verified
- Clean build (`cmake --build build --target sol`), no warnings.
- All 16 ctest suites pass, before and after.
- Standalone concurrency harness (scratch-only, never touched the user's
  real `~/.sol`): 6 processes × 200 concurrent `sol_settings_save` calls
  against an isolated `HOME` — final file always exactly one complete,
  internally-consistent write, zero leftover `.tmp*` files.
- Real two-process test: launched the actual built `sol` binary against an
  isolated fake `$HOME`, saved settings from a second process while it ran
  — confirmed (via temporary stderr instrumentation, since screen capture
  is unavailable in this shell — see [[screencapture_unavailable]]) that
  the running instance's watcher fired and reloaded live. Instrumentation
  was removed before the final build.
- NOT verified: the visual result of a live theme/appearance change while
  two real GUI windows are both on screen simultaneously (would need
  screen capture / a second display, unavailable here).

## Notes
- `sol_file_watcher_set_root` requires the target to be a directory
  (`sol_platform_get_path_info(...).is_directory`), so this watches the
  whole `~/.sol` dir and filters by basename in the drain function, rather
  than trying to watch `settings.json` directly.
- `SolFileWatcher` has no shared/global state (`calloc`'d per handle,
  queue is per-instance-mutex-guarded) — confirmed safe to instantiate a
  second one independent of the explorer watcher.
- `autosave_enabled` has no live-applied side effect beyond the flag value
  itself (the autosave sweep reads `app->settings.autosave_enabled` fresh
  every frame), so it's just copied on reload with no extra apply call.
