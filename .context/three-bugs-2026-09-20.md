# Three reported bugs fixed — 2026-09-20 (later session)

## 1. "Open Folder" never reaches Recents

**Symptom**: app starts at the user's home directory; using File > Open
Folder to pick a different directory never showed up in Recents (only
opening a brand-new project via the switcher did).

**Root cause**: `sol_recent_record` (main.c) was only ever called from
`sol_project_create`, i.e. only when a *new* project runtime is created.
"Open Folder" (`sol_on_menu_open_folder` → `sol_on_picker_folder_chosen`)
instead calls `sol_set_explorer_root` directly, which repoints the
*existing* project's tree root in place — a completely different code
path that never touched recents.

**Fix**: moved the record call into `sol_set_explorer_root` itself
(main.c:344), gated on success (`ok`) and `app->host` being set. Every
path that repoints an explorer root — Open Folder, the file-tree
context-menu "Open" on a directory, the CLI directory argument, the
explorer-toggle cwd fallback, and initial project creation — funnels
through this one function, so all of them now reach recents consistently.
`sol_recent_record`'s existing dedup-and-move-to-front logic makes the
now-occasional double-record (once via the new hook, once via
`sol_project_create`'s own explicit call right after) harmless.

## 2. Project/session tab bar doesn't follow the theme palette

**Symptom**: switching themes recolors the status bar, buffer tabs, tree,
editor, etc., but the top project-session tab strip stays on whatever
colors it last had (effectively frozen on the Glass fallback's hardcoded
`style.h` values).

**Root cause**: `style.h`'s `.project-tabs`/`.project-tab`/
`.project-tab-active`/`.project-tab-label` rules are the Glass **fallback**
theme only. Real theming works by `plugins/sol-plugin-themes` generating a
complete replacement stylesheet per curated theme (see
[[project_widget_style_system]] / `.context/theme-system.md`) — and that
generator's `build_theme_css` (plugin.c) styled `.status-bar`,
`.buffer-tabs-row`, `.ca-titlebar`, etc. but never emitted any
`.project-tabs*` rule at all. So every curated theme silently left it on
stale/fallback colors while correctly recoloring everything else.

**Fix**: added a `.project-tabs`/`.project-tab`/`.project-tab:hover`/
`.project-tab-active`/`.project-tab-label` block to `build_theme_css`
(plugins/sol-plugin-themes/src/plugin.c, right after the `.status-bar`
block), using `chrome` for the strip background (same chrome tier as
`.status-bar`/`.ca-titlebar` — [[bar_consistency_full_width]] already
established these two bars should read as one repeated element) and the
same `panel`/`hover`/`selected`/`theme->text`/`theme->muted` tokens
`.buffer-tab` already uses, so switching projects now reads consistently
with switching buffers under every theme.

## 3. Git plugin's Commit button "not clickable predictably"

Investigated via a deep trace of Causality's click dispatch (see
[[git-source-control-plugin]] and the historical
`buffer-tab-close-stability.md` sibling-bug note this closes out). Two
real, independently confirmed causes — the originally-suspected
`GitActionContext`/`action_count` reentrancy pattern was traced in full
and is **confirmed unreachable** (fixed-array storage never invalidates
addresses; single-button-per-event dispatch; handlers copy `context`
fields before triggering any rebuild) — see that file for the full proof.

**Real cause A — no visual disabled state (git-specific, primary cause)**:
`.scm-primary-action`/`.scm-action`/`.scm-icon-action`/`.scm-remote-action`
etc. had zero `:disabled` CSS anywhere (neither `style.h` nor the theme
generator), even though Causality's CSS engine fully supports the
`:disabled` pseudo-selector. A correctly, deliberately disabled Commit
button (no staged changes, blank/whitespace-only message, or a task still
running) rendered pixel-identical to a clickable one — the click was
correctly refused with zero feedback, which reads exactly as "sometimes
just doesn't work." Fixed by adding dimmed `:disabled`/`:disabled:hover`
rules for the whole `.scm-*` action-button family in both `style.h`
(Glass fallback) and the theme generator (so it survives theme changes
too).

**Real cause B — app-wide menu-bar click-eating bug (Causality, not
git-specific)**: in `ca_widget_input_pass`
(vendors/causality/causality/src/ui/widget.c), the menu-bar dropdown block
set `click_consumed = true` **unconditionally** the instant any menu was
open (`mb->active_menu >= 0`), *before* checking where the click landed —
unlike the context-menu block right below it, which only consumes a click
that actually lands inside the menu (explicit comment there: "if it lands
outside we just close the menu without consuming"). Repro: open any
title-bar menu, then click any button anywhere else in the app (Commit
included) — that click only dismisses the menu; the button's own action
never fires, silently, with the menu closing as the only visible feedback.
Fixed by mirroring the context-menu's pattern: `click_consumed` now only
flips true when the click actually lands on a menu item, a separator, or
switches to another menu header; a click landing entirely outside the
menu system still closes it but leaves the click to reach whatever is
underneath.

## Files changed
- `sol/src/main.c` (recents fix)
- `plugins/sol-plugin-themes/src/plugin.c` (project-tabs theming + scm
  disabled-state theming)
- `sol/src/ui/style.h` (scm disabled-state fallback CSS)
- `vendors/causality/causality/src/ui/widget.c` (menu-bar click-consume fix)

## Verification
- `cmake --build build -j` clean, zero new warnings.
- Full `ctest`: **20/20 passed** (including `causality_shader_cache_tests`,
  which had been intermittently failing all session — confirmed flaky/
  environment-dependent, not caused by any of these changes, since it now
  passes with no changes to that test or its subject).
- Format-string argument count in the new theme-generator `css_append`
  call manually verified (6 `%s` / 6 arguments) since `css_append` has no
  `printf`-format compiler annotation to catch a mismatch automatically.
- Not interactively verified end-to-end in the running app (no
  screen-recording permission in this shell — see
  [[screencapture_unavailable]]); root causes were traced to specific,
  cited lines and the fixes directly address the mechanism, not just the
  symptom.
