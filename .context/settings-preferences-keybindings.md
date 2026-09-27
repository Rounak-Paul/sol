# Settings window: Preferences + Keybindings tabs (2026-09-27)

## Tabs (`sol/src/ui/settings_window.c`, 760×540)
`SolUISettingsTab` (sol_ui_system.h): THEME / PREFERENCES / KEYBINDINGS.
`sol_ui_system_open_settings_window(ui, tab)` opens, or switches the open
window (one per SolUISystem) to `tab`. `sol_ui_settings_window_open(ui, tab)`
now takes only the ui — the window reads settings/registries/signals off it.

Menus (workspace.c): Sol > Settings... (single item, opens on Theme tab; tabs switch inside the window);
Sol > Autosave: On/Off (kept); View > Show Hidden Files: On/Off (new).

## Preferences (settings.json, `~/.sol`)
| field | JSON | effect |
|---|---|---|
| autosave_enabled | editor.autosave | existing |
| autosave_delay (0.5–10 s, def 1.5) | editor.autosave_delay | replaced `SOL_AUTOSAVE_DEBOUNCE_NS` in main.c |
| caret_blink (def true) | editor.caret_blink | off ⇒ solid caret AND no per-frame sig_buffer_rev bump in sol_ui_on_frame |
| show_hidden_files | explorer.show_hidden | `sol_file_tree_set_show_hidden` (re-scan) + `sol_file_picker_set_default_show_hidden` |

`sol_ui_system_apply_preferences(ui)` is the single push point (tree, picker
default, buffer rev, `ui->sig_prefs_rev`, menu labels). Called by the prefs
tab, menu toggles, `sol_ui_system_set_settings`, and the settings watcher
drain (field diff). Parser helpers `jp_field_bool/jp_field_float` consume a
mistyped value instead of desynchronising the object (old jp_bool/jp_float
paths left the cursor on bad values).

## Command registry refactor (command_flow.c) — important
Before: bindings.conf registered flows before plugins; plugin registration
(git.*) then overwrote user chords, a chord collision *deleted* the other
command (losing plugin callbacks), and "clear + reload" would drop plugin
commands entirely.

Now two layers:
- **Owner registration** `sol_ui_system_register_command_flow`: sets label,
  callback, `default_sequence` (may be empty ⇒ palette/menu-only command),
  `owned = true`.
- **User keymap** `sol_ui_system_set_keymap_override(ui, action, seq, mods, len)`
  (len 0 = unbind). Stored in `ui->keymap_overrides`, applied now and on any
  later registration of that action. Unregistered actions become unowned
  event-driven flows (all template actions like buffer.new are unowned).
- Effective chord `sequence_length == 0` ⇒ unbound but still invokable
  (menus / `sol_ui_system_invoke_command`). All matchers already require
  `sequence_length > prefix_length`, so unbound flows never match.
- Collision rule (`sol_ui_flow_assign_chord`): override beats anything;
  a default never displaces a user chord; default vs default ⇒ later wins.
  Loser is unbound, not removed.
- `sol_ui_system_reset_keymap`: drop overrides, remove unowned flows,
  restore owned defaults in registration order.
- `SOL_UI_MAX_COMMAND_FLOWS` 64 → 128 (54 live with git plugin).

## bindings.conf (sol_config.c)
- New directive `unbind <action>`. Loader resets leader to ctrl first, then
  bind/unbind → keymap overrides.
- `sol_config_parse_chord` / `sol_config_format_chord` (leader-relative text
  "b shift+s"; rejects '#', ctrl+ steps, steps using the leader, >8 steps).
- `sol_config_save_binding` / `sol_config_save_leader` rewrite only the
  affected line(s), preserve everything else, atomic write
  (`sol_config_write_atomic`). Leader change rewrites literal `bind ctrl ...`
  lines to `bind L ...`. `sol_config_reset_bindings` writes the template.
- `sol_config_default_bindings` parses the template (row "Default" button).

## Live reload
Keybindings tab writes bindings.conf then publishes
`SOL_EVENT_BINDINGS_CHANGED` (sol_event.h) on the project bus → main.c
`sol_reload_bindings` = reset_keymap + load. Other projects / instances
reload via `sol_drain_settings_watcher` (now also matches `bindings.conf`).
Tab validation rejects duplicate chords AND prefix overlaps (the shorter
exact match fires first in the leader dispatcher, so the longer is dead).

## Styling
New classes in style.h: sw-btn(+:disabled), sw-btn-label, sw-scroll,
sw-bind-row/action/leader/input/group/status(-error), sw-setting-hint,
sw-setting-label-wide, sw-section-title-spaced, sw-select-narrow. Theme
generator (plugins/sol-plugin-themes, submodule) now also themes the
previously unthemed sw-section-title/sw-setting-label/sw-tab-label text;
`.sw-tab-label-active` is emitted last so it wins the equal-specificity
cascade. Retro: sw-btn raised/pressed, sw-bind-input sunken.

## Tests
`sol_config_tests` (tests/test_config.c, isolated $HOME): chord round trip,
malformed chords, default table, surgical save, unbind+reload, leader rebase,
prefs round trip + bad values. test_command_flow: override-beats-later-default,
palette-only registration. test_file_tree: show_hidden toggle.
Runtime-verified (stderr instrumentation, isolated HOME): all three tabs
render; git.* overrides/unbind survive plugin load. NOT visually verified
(no screen capture) — layout/colours of the new tabs are unchecked.
