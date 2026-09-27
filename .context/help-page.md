# Help page (L h → help.open) — 2026-09-27

- `help.open` (default chord leader+h, label "Help") registered in code by
  `sol_register_help_command_defaults` (main.c) so it works with pre-existing
  bindings.conf; also in the template (`bind L h help.open`) and a **Help >
  Sol Help** title-bar menu item (menu_order 1000, after Plugins 900).
- `sol_open_help` (main.c): builds the doc, closes the previous help buffer
  (`app->help_buffer`) and opens a fresh one in the active leaf — regenerated
  every time so it reflects keymap changes.
- Content: `sol_ui_system_build_help_document` (src/ui/help_page.c) → Markdown.
  Cheatsheet generated from the live registry (`ui->command_flows`, effective
  leader via `sol_config_modifier_name`), grouped by action prefix (known
  categories ordered in `HELP_CATEGORIES`, plugin prefixes after, capitalised),
  columns Keys | Action | Description. Description = `sol_config_action_description`
  (parses the bindings template's "#   <action>  <desc>" block — the single
  source of built-in action docs) else the registered label if ≠ action.
  Followed by a static guide (editing keys, terminal, projects, customising).
  Buffers don't soft-wrap: keep every prose source line short (~60 chars).
- Read-only document mode (sol_text_buffer): `sol_text_buffer_open_document`
  (no source path, markdown flag explicit), `sol_text_buffer_is_read_only`;
  every mutation/save/reload entry point refuses (save sets "read-only
  document"); main's buffer.save no-ops silently on read-only buffers.
- Git plugin commands now register real labels (were the action ids), which
  also improves the which-key popup.
- Tests: sol_config_tests `test_action_descriptions` (all defaults documented),
  sol_text_buffer_tests `test_read_only_document`.
