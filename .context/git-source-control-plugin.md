# Git Source Control Plugin

## 2026-08-30 commit graph view added to History tab

Added a compact branch/merge graph to the History tab (in-sidebar, not a
separate wide view — confirmed with the user first). Three layers:

**Model** (`git_plugin.h` + `git_model.c`):
- `GitCommitEntry` gained `parents[GIT_MAX_PARENTS][41]` (cap 2 — octopus
  merges beyond that are rare and the graph only needs enough to draw
  merge lines, not full ancestry) + `parent_count`, plus computed `lane`
  (int) and `through_lanes` (uint32_t bitmask of which lanes have an
  active line through this row).
- `git_model_history()`'s `git log` format string gained `%P` (parent
  hashes, space-separated) between date and subject, parsed by new
  `git_parse_parent_hashes()`.
- New `git_model_layout_graph(GitHistory*)`: single forward pass over
  commits in git-log's native newest-first order. Tracks which commit
  hash each "lane" is currently waiting for; a commit claims the lane
  already waiting for it (or opens a new one — a fresh branch tip);
  records every currently-active lane as "through" this row before
  updating; first parent continues the commit's own lane, additional
  parents (merges) each claim an existing waiting lane or open a new one;
  trailing free lanes are trimmed so a closed branch doesn't hold a
  column open forever. Prototyped standalone in /tmp against two hand-
  built scenarios (a feature-branch-then-merge, and two independent
  branch tips that never reconverge) BEFORE touching the real file, since
  lane algorithms are exactly the kind of thing that's easy to get
  subtly wrong — both scenarios produced the visually-correct lane
  assignments (merge commit stays on mainline lane 0, feature branch
  gets lane 1, converges back to 0; two unrelated tips share lane 0
  sequentially rather than each claiming a lane forever). Ported
  verbatim into git_model.c after verification, and turned both
  prototype scenarios into real tests in test_git_model.c
  (test_graph_layout_merge, test_graph_layout_unrelated_branches).

**Rendering** (`plugin.c`, `git_render_history`):
- `scm-commit-row` changed from vertical (subject-over-meta) to
  horizontal: a `git_render_graph_gutter()` column on the left, then a
  new `scm-commit-info` vertical wrapper holding the existing subject +
  meta-row. This is a structural button-content change of exactly the
  kind that caused the tab-divergence bug earlier in this session — but
  here BOTH children are manually-added, explicitly-styled ca_div/ca_text
  (never Ca_BtnDesc.text), so there's no risk of the two-rendering-paths
  divergence that bit the Changes tab.
- Graph gutter is plain axis-aligned absolutely-positioned divs (a
  full-row-height 2px vertical bar per active lane, a centered dot at the
  commit's own lane) — no rotation/diagonal math at all, deliberately,
  after the diagonal-line idea was considered and rejected as too much
  unverified pixel math for how this session has gone. Gutter width is
  computed per-render from the actual max lane span in view
  (`git_graph_lane_span`), capped at `GIT_GRAPH_MAX_VISIBLE_LANES = 5` so
  a heavily-branched history can't push commit text off the narrow
  sidebar.
- Merge commits get a distinct dot color (`scm-graph-dot-merge`); the
  active-lane line segment on a row is distinguished from merely-passing-
  through lanes (`scm-graph-line-active`).
- Added theme-CSS mapping for `.scm-graph-line-active`/`.scm-graph-dot-
  merge` in sol-plugin-themes/src/plugin.c (theme->primary/accent) —
  learned this lesson from the earlier follow-up: any new `.scm-*` class
  needs a matching entry there or it's stuck on style.h's static fallback
  regardless of active theme. Re-verified placeholder/arg count with the
  same python script each time (21 == 21).
- Verification: `sol_plugin_git`, `sol_plugin_themes`, `sol` all build
  clean; full ctest 14/14 passes including the 2 new graph-layout tests;
  `./bin/sol` launches and stays alive.

## 2026-08-30 tab divergence root-caused + refresh icon swapped (follow-up #4)

User provided a tight crop of the header, which finally showed the actual
symptom precisely: (1) the refresh glyph itself looked visually malformed
(gap in the arrow, asymmetric weight) rather than being cut off by a box
edge — ruling out every box/font-size theory from follow-up #3; (2) the
"Changes" tab rendered strikingly larger, bolder, and shifted up compared
to History/Branches. Both looked identical across two more rebuild+relaunch
cycles the user confirmed were genuinely on the latest binary, which ruled
out staleness as an explanation for why earlier fixes "did nothing."

Root cause for (2), finally confirmed by re-deriving from
`vendors/causality/causality/src/ui/layout.c`'s `content_size()`: a
button's own `Ca_BtnDesc.text` is painted directly onto the button node by
`paint.c`'s `CA_WIDGET_BUTTON` case, completely independent of the flex
child-layout system. The "Changes" tab had been given a dot-badge CHILD
(`ca_text` for the unread-changes indicator) alongside its own separately
manually-added child `ca_text` label — meaning, unlike History/Branches
(plain `Ca_BtnDesc.text`, zero children), Changes had two different
rendering/sizing systems in play on the same button, and got real,
measurable divergence from its siblings as a result — this was not a CSS
values problem, an icon-scale problem, or a caching/staleness problem, all
of which were tried and ruled out across follow-ups #2 and #3.
- **Decision, explicitly approved by the user rather than guessed at
  further**: dropped the dot-badge feature entirely and put all three tabs
  back on the identical simple path (`git_render_button()`, plain
  `Ca_BtnDesc.text`, no children). Removed the now-dead `.scm-tab-label`,
  `.scm-tab-label-active`, `.scm-tab-dot` CSS (added in follow-up #3,
  never actually needed) from style.h AND their mapping in
  `sol-plugin-themes/src/plugin.c`'s `build_theme_css` (re-verified
  placeholder/arg count with the same python counting script each time —
  19 == 19 after removal).
- **Lesson**: when a Causality button needs a child element alongside its
  label (icon buttons, badges, secondary content), the label MUST also be
  a manually-styled child `ca_text` — never `Ca_BtnDesc.text` — because the
  two rendering paths (`paint_text` directly on the button node vs. real
  flex children) are NOT interchangeable and WILL visibly diverge in size/
  position once one sibling has children and another doesn't share the
  same structure. `.buffer-tab`/`.buffer-tab-close` in workspace.c always
  uses the child-ca_text convention for exactly this reason — that was
  the correct model, and the fix that actually needed making was simpler
  than either broken form: don't introduce structural asymmetry between
  otherwise-identical sibling elements at all if it can be avoided.
- Root cause for (1) never confirmed with certainty — investigated the
  icon-face scale/raise compensation (`CA_FONT_SYMBOLS_ICON_SCALE=0.78`,
  `CA_FONT_SYMBOLS_ICON_RAISE_EM=0.11`, uniform across all icons, ruled
  out as icon-specific), atlas baking (dynamic per-size, ruled out
  quantization), and confirmed `CA_ICON_NF_COD_REFRESH` (U+EB37) is used
  NOWHERE else in this codebase, so there was no working precedent to
  compare against. Rather than keep debugging a single glyph blind,
  swapped it for `CA_ICON_NF_FA_REFRESH` (Font Awesome's refresh
  codepoint, U+F021) — a different icon family/glyph shape entirely — as
  a pragmatic sidestep. If this specific glyph is malformed for a reason
  intrinsic to this rendering engine (not yet identified), a different
  glyph shape from a different source font is likely to avoid it; if it's
  NOT glyph-specific (e.g. some structural issue with `.scm-header-icon-
  action`'s specific box), swapping the icon proves nothing and the box/
  button structure needs the same "single unified path" scrutiny the tabs
  got. Ask the user to re-screenshot after this change specifically to
  find out which.
- Also removed the now-dead `bool large` parameter from
  `git_render_icon_button()` (a leftover from the abandoned two-size
  20px/16px icon-button experiment in follow-up #3) and all 9 call sites.
- Verification: `sol_plugin_git`, `sol_plugin_themes`, `sol` all build
  clean, full ctest 14/14 passes, `./bin/sol` launches and stays alive.
  **User confirmed fixed** after this round — both the tab divergence and
  the refresh icon are resolved. The structural fix (no child-vs-plain-text
  asymmetry between sibling buttons) was the one that actually mattered;
  the icon swap to Font Awesome's refresh glyph is unproven as to *why* it
  helped (never got a confirmed root cause for the Codicon glyph itself)
  but the combination is what the user is now running clean.

## 2026-08-30 icon clipping still present + duplicate-looking file rows (follow-up #3)

A second screenshot showed the refresh icon STILL clipped after follow-up #2's
fix (which introduced a 20px/12px "large" icon-button variant for header/
remote actions) — proof that guessing bigger numbers doesn't fix a clipping
bug whose actual cause was never confirmed. Investigated the rendering
pipeline properly this time instead of iterating on pixel values:
- `vendors/causality/causality/src/renderer/font.c` confirms icons render
  through a dedicated Nerd Font `icon_face`, with a uniform, deliberate
  `CA_FONT_SYMBOLS_ICON_SCALE = 0.78` and `CA_FONT_SYMBOLS_ICON_RAISE_EM =
  0.11` compensation applied to every icon glyph in the app — so icon
  glyphs are already shrunk 22% and raised slightly to match text
  cap-height; this rules out "icons render bigger than font-size implies"
  as the cause, and rules out anything glyph-specific to refresh/sync
  since the compensation is uniform across all icon codepoints.
- `ca_text` (widget.c ~line 1022) defaults an unset-height label to a flat
  16px line-box regardless of font-size — not itself a clipping cause
  (smaller content centered in a bigger button is fine) but confirms
  button-box size was never the actual constraint being fought.
- Default `overflow` on any node is visible (not clipped) unless CSS sets
  `overflow: hidden` explicitly (style.c) — so "clipped" in the screenshot
  is more likely uncontained visual overflow past the button box than a
  hard clip rect, i.e. the earlier 16px/20px box-size theories were solving
  a problem that doesn't happen the way assumed.
- Root finding: `.buffer-tab-close`/`.buffer-tab-close-icon` (16px box,
  11px glyph) is the ONLY icon-button geometry in the entire app with a
  confirmed non-clipping track record (visible working in both screenshots
  — titlebar close, buffer tab close). `.welcome-btn`/`.buffer-tab-close`
  icons that render fine elsewhere put the icon glyph INLINE in the same
  ca_text as the label text (`CA_ICON_NF_COD_NEW_FILE " New Buffer"`), not
  as a bare icon-only ca_text in a tiny fixed box — but `buffer-tab-close`
  IS a bare icon-only ca_text in a 16x16 box and works, which is the
  closest analog to what this panel needs.
- Fix: collapsed the two-size icon-button system back down to ONE size for
  every `.scm-*` icon button (header, remote, and row actions alike) —
  16px box / 11px glyph, copying `.buffer-tab-close`'s exact numbers with
  zero deviation. Stopped inventing new pixel values entirely; if this
  still clips, the next step is not another guessed number but getting
  actual pixel-level evidence (screenshot from the user showing the
  precise clipped glyph, or building a Vulkan frame-capture path — see
  [[screencapture_unavailable]]) before touching these numbers again.
- Separately, the same screenshot showed `plugin.c` listed twice under
  Changes. Confirmed via `git status --porcelain=v2 -z` this was NOT a
  parser bug — porcelain-v2 emits one record per file, and there really
  were two distinct files pending: `plugins/sol-plugin-git/src/plugin.c`
  and `plugins/sol-plugin-themes/src/plugin.c`. The "filename only" display
  added in the first rework collapsed both to the same bare "plugin.c"
  label with no way to tell them apart at a glance (the hover tooltip
  still showed the right full path, but that's not discoverable without
  hovering). Fixed with progressive disambiguation: `git_display_name()`
  starts at the basename and, only for files whose basename collides with
  another currently-visible file, grows leftward one path segment at a
  time until the shown suffix is unique versus every other visible file
  (verified with a standalone test program — the first attempt at this,
  grabbing exactly one extra parent segment, still collided when both
  colliding files shared the same immediate parent name "src"; the fix
  needed to walk back segment-by-segment, checking uniqueness after each
  step, not assume one extra segment is always enough).
- Verification: `sol_plugin_git` and `sol` build warning-free, full ctest
  14/14 passes, `./bin/sol` launches and stays alive 5s+. The icon-clipping
  fix specifically has NOT been visually re-confirmed by the user yet as of
  this write-up — say so plainly rather than claiming it's fixed, since
  the previous "fix" in follow-up #2 also looked correct on paper and
  wasn't.

## 2026-08-30 icon clipping + live-theme integration (follow-up #2)

User shared a screenshot showing the header refresh icon visibly clipped, and
asked for the color scheme to be "slightly better and dynamic". Two distinct
root causes:

1. **Icon clipping**: `.scm-icon-action` etc. sized the button box flush to
   the glyph's font-size (16px box / 11px text, same ratio as
   `.buffer-tab-close`). Round Nerd Font glyphs (refresh/sync spinner) have
   more visual ink near their em-box edge than a flat "x" close glyph, so
   the same ratio that works for `.buffer-tab-close-icon` clipped for
   refresh. Fixed by splitting into two icon-button sizes: `.scm-icon-action`
   stays 16px/11px for dense in-row actions (open/stage/unstage/discard);
   `.scm-header-icon-action`/`.scm-action-icon` bumped to 20px/12px for
   panel-level actions (refresh, close, fetch, pull, push) which have more
   headroom in the taller header/remote-actions chrome anyway.
   `git_render_icon_button()` in plugin.c gained a `bool large` parameter
   (all header/remote call sites pass `true`, row actions pass `false`)
   selecting between `.scm-icon-glyph` and a new `.scm-icon-glyph-lg` class
   on the inner `ca_text`. Also found and fixed a real overflow bug in the
   same screenshot: the Changes/History/Branches tab row used fixed padding
   and the "Changes (6)" label (added in the first rework) pushed the row
   past the sidebar width, clipping "Branches" to "Branc". Fixed by making
   `.scm-tab`/`.scm-tab-active` `flex-grow:1` (always divides the actual
   available width evenly, can't overflow) and replacing the count-in-label
   with a small `.scm-tab-dot` badge next to "Changes" instead (the count
   still shows in the section header below).

2. **Not integrated with the live theme system** — this was the deeper
   finding. `plugins/sol-plugin-themes/src/plugin.c` generates a full CSS
   override string per theme (`build_theme_css`, ~40 curated palettes,
   `sol_plugin_register_theme`) that already targeted a handful of original
   `.scm-*` classes (`.scm-toolbar`, `.scm-repository`, `.scm-commit-box`,
   `.scm-section-header`, `.scm-file-row:hover`, `.scm-commit-row:hover`,
   `.scm-branch-row:hover`, `.scm-commit-input`, `.scm-branch-input`,
   `.scm-branch-row-current`, `.scm-root`, `.scm-view`) with the active
   theme's semantic colors (`theme->primary/accent/secondary/success/
   danger/warning/text/muted`). Every class the panel rework added or
   renamed (`.scm-primary-action`, `.scm-icon-action`, `.scm-tab-active`,
   `.scm-status-*`, `.scm-sync-*`, `.scm-title-icon`, `.scm-branch-icon`,
   etc.) postdated that block and was never wired in, so the panel stayed
   on style.h's static fallback hex colors regardless of which of the 40
   themes was active — this is why it visually clashed with the rest of
   the app once a non-default theme (the screenshot showed an olive/green
   theme) was selected. Fixed by adding a second `css_append` block mapping
   every new/renamed `.scm-*` class to the correct semantic role: primary/
   accent for interactive chrome (title icon, branch icon, active tab, CTA
   buttons, busy spinner), success/warning/danger/accent for git status
   colors (added/modified/deleted/renamed/conflict, ahead/behind, error).
   `css_append` is `vsnprintf`-based with exact positional `%s` placeholder
   count matching argument count — got this wrong on the first pass (20
   placeholders, 21 args) and had to recount by hand with numbered
   inline comments; verify placeholder/arg count with a script before
   trusting any future edit here, since a silent off-by-one just shifts
   every subsequent color one class to the left with no compiler error
   (variadic wrapper, not directly type-checked against the literal).
- **Lesson for next time touching this panel**: any new/renamed `.scm-*`
  class needs a matching entry added to `plugins/sol-plugin-themes/src/
  plugin.c`'s `build_theme_css`, or it silently falls back to whatever
  static color is in style.h and drifts from the active theme. Check that
  file FIRST, before assuming style.h's own colors are what actually
  render — style.h is the fallback/default, this plugin is the live
  override for anyone who isn't on the default theme.
- Verification: `sol_plugin_themes`, `sol_plugin_git`, and `sol` all
  build warning-free; full ctest 14/14 passes; `./bin/sol` launches and
  stays alive (a `build_theme_css` truncation would fail that theme's
  `css.valid` silently, not crash, so this doesn't prove zero truncation
  — the hand-verified placeholder/arg count is what backs correctness here).

## 2026-08-30 visual-language alignment fix (follow-up to the panel rework)

The reworked panel had its own disconnected micro-design system instead of
matching the app's established scale, found by auditing `.tree-row`/`.tree-name`
(12px text, 22-24px rows), `.search-result-path`/`.search-result-line`
(13px/11px), `.pm-btn-text` (12px via `SOL_UI_BOOT_FONT_SIZE_PX_CSS`), and
`.buffer-tab-close` (16x16px icon button) as the reference conventions:
- Retokenized nearly every `.scm-*` font-size from ad-hoc 8/9/10px values to
  `SOL_UI_BOOT_FONT_SIZE_PX_CSS` (12px, the app's shared primary-text token)
  for names/labels/headings, and 11px (matching `.search-result-line`) for
  secondary/meta text — instead of inventing its own smaller scale.
- Icon-only buttons (`scm-icon-action`, `scm-header-icon-action`,
  `scm-action-icon`) shrunk from an invented 22px grid to 16x16px, matching
  `.buffer-tab-close` exactly, since that's the app's one existing icon-button
  precedent.
- Row heights bumped slightly (file row 26->28px, commit row 44->46px,
  branch row 36->38px, tabs 28->30px) to give the now-larger text proper
  breathing room, matching `.tree-row`/`.search-result` row-height range.
- Decorative (non-semantic) icon tints removed: the header title icon and
  branch-name icon were colored blue (`#7b8dc4`) for no reason — the app's
  convention (see `.buffer-tab-close-icon`) is muted grayscale for
  non-semantic icons, reserving color for meaningful state (git status
  letters, ahead/behind, the busy-spinner blue which matches
  `.status-bar-badge-key`'s existing blue-for-active-state precedent).
- Root cause: the panel was designed in isolation against no specific
  reference, inventing its own font scale and icon-button size rather than
  reusing `SOL_UI_BOOT_FONT_SIZE_PX_CSS` and the tab-close icon-button
  pattern that already existed. When touching any panel's CSS in this
  codebase, check `.tree-row`/`.search-result`/`.pm-btn`/`.buffer-tab-close`
  first for the prevailing text-size and icon-button-size convention rather
  than picking new pixel values.

## 2026-08-30 panel UI rework

Full visual/interaction rework of the panel in `plugins/sol-plugin-git/src/plugin.c`
(rendering) and the `.scm-*` rules in `sol/src/ui/style.h` (styling). Model/task
layer (`git_model.c`, `git_process.c`, `git_view.c`) untouched.

- File-row click semantics inverted: the row itself now fires `GIT_UI_DIFF`
  (was `GIT_UI_OPEN`). A dedicated small icon button (`CA_ICON_NF_COD_GO_TO_FILE`,
  "Open File" tooltip) opens the file for editing instead. Achieved by nesting an
  inner `ca_btn_begin`/`ca_btn_end` icon button inside the outer row's own
  `ca_btn_begin` — same pattern already proven by `buffer-tab`/`buffer-tab-close`
  in workspace.c. Verified against Causality's click dispatch
  (`vendors/causality/causality/src/ui/widget.c` ~line 4673): it always picks the
  smallest-area hit button at the top z-index, so the small nested icon button
  correctly wins over the larger row button — no explicit stopPropagation needed.
- File rows show only the basename (`git_basename()`, new static helper) with the
  full repository-relative path as a `ca_tooltip` shown on hover; renamed files
  show `old-name -> new-name` (basename form) inline plus the full rename in the
  tooltip. `original_path` was already parsed by git_model.c but previously never
  surfaced in the UI.
- Row actions (open/stage/unstage/discard) converted from text-label buttons to a
  fixed-width 22px icon-button rail (`scm-icon-action`) so rows align into clean
  vertical columns regardless of file name length — this was the main "alignment"
  complaint, since the old `scm-file-open` button was flex-grow with a raw path
  string, making row actions drift per row.
- Header/remote actions (Refresh, Close, Fetch, Pull, Push) converted to icon
  buttons with hover tooltips (`git_render_icon_button`, new helper). Ahead/behind
  now shown as arrow-icon + count, hidden when zero instead of always printing
  "up 0 down 0".
- Empty/clean states (no changes, no commits, not a repository) get a centered
  icon + message treatment (`scm-clean-state`) instead of a bare text line.
- Commit button shows staged count inline ("Commit (3)"); Changes tab shows total
  file count ("Changes (7)"). Empty staged/unstaged groups are omitted entirely
  (previously showed a dangling "CHANGES 0" header with nothing under it).
- Discard confirmation now names the file being discarded and distinguishes
  untracked-delete wording from tracked-discard wording.
- All icon glyphs come from `ca_icons.h`'s Nerd Font codepoints (CA_ICON_NF_COD_*);
  no new font/asset dependency.
- Verification: `cmake --build . --target sol_plugin_git`, `--target sol`, and
  `--target sol_git_plugin_tests` all succeed; full `ctest` 14/14 pass; `./bin/sol`
  launches cleanly with `plugins=13` loaded and no CSS/plugin-load errors on
  stderr. Screenshot-level visual proof unavailable — see
  [[screencapture_unavailable]].

## Goal

Implement Git as a production-ready Sol feature with a native source-control UI,
keyboard commands, asynchronous operations, and safe repository mutation flows.

## Current Architecture

- Plugins load through `SolPluginCtx` and can register commands, events, jobs,
  services, and status-bar segments.
- `SolUISystem` owns the reactive workspace layout. Its builders subscribe to
  Causality signals; worker threads must publish snapshots for UI-thread adoption
  and wake the Causality instance.
- The existing public plugin API can open custom editor buffers but cannot add a
  first-class workspace panel. A host-owned panel contribution API is required.
- `sol_file_tree_root()` is the canonical workspace root. Panel visibility must
  remain separate from repository discovery state.
- A stale, untracked build artifact contains an earlier Git plugin that rendered
  status/diff/log/blame as custom buffers. Its source is absent and it is not the
  implementation baseline.

## Implementation Plan

- Extend `SolUISystem` and `SolPluginCtx` with lifecycle-managed side-panel
  contributions and reactive invalidation.
- Add `Plugins/sol-plugin-git` with portable process execution, repository model,
  porcelain parsing, async refresh/mutation jobs, and native Causality rendering.
- Support status, stage/unstage, discard confirmation, commit, diff, history,
  blame, fetch, pull, push, branch checkout/create, refresh, and file opening.
- Register command-palette/leader actions and a branch status-bar segment.
- Add focused parser/process tests, then build and run the full test suite.

## Invariants

- Git subprocesses never block the UI thread.
- Worker threads never mutate Causality nodes or UI-owned collections directly.
- Repository snapshots have one owner and are adopted atomically on the UI thread.
- Paths are passed as argv entries with `--`; no shell command construction.
- Destructive discard/clean operations require explicit confirmation.
- Plugin unload waits for owned work and removes all UI contributions.
- The left sidebar is a dynamic content slot. Showing the explorer deactivates
  the current plugin panel, while showing a plugin panel replaces the explorer
  without destroying either content provider's state.
- Mouse and keyboard access share command actions. Title-bar menu contributions
  reference registered command IDs, and plugin-owned items are removed before
  plugin commands and code are unloaded.
- No commits or pushes are made by this task.

## Progress

- Repository, plugin ABI, workspace reactivity, and stale Git artifact audited.
- Added lifecycle-managed `SolUISidePanel` contributions with reactive rebuild,
  UI-thread tick, wake, visibility, ownership checks, and plugin auto-cleanup.
- Added the `sol-plugin-git` source tree with portable non-interactive process
  execution, porcelain-v2 parsing, repository discovery, event-driven refresh, and
  serialized asynchronous task ownership.
- Implemented stage/unstage, stage all/unstage all, guarded discard, commit,
  fetch, fast-forward pull, push with first-upstream setup, repository init,
  branch creation/checkout, history, file diff, commit show, and blame.
- Added native source-control panel UI, branch status segment, keyboard command
  flows, progress/error states, clean-tree state, and colorized read-only views.
- Corrected application shutdown order so plugins unload before UI, syntax, and
  Causality resources are destroyed.
- Added parser/process tests and side-panel lifecycle tests. Top-level CTest now
  discovers all targets.
- Verification: full build passes; plugin sources pass `-Wall -Wextra
  -Wpedantic`; all 10 CTest targets pass with normal temp-file access; native
  Sol startup reports 11 plugins loaded and input ready.
- Replaced Unix fork/chdir execution with `posix_spawn` and explicit `git -C`
  repository selection. Windows uses the same `git -C` argument flow without a
  process current-directory dependency. This removes the recurring launch/setup
  exit 126 path and preserves useful Git errors for invalid repository paths.
- Removed the three-second visible status poll that repeatedly rebuilt the panel.
  Refresh now occurs on panel open, explicit refresh, workspace-root changes, and
  after Git mutations.
- Regression verification: strict subprocess compilation passes, the Git plugin
  tests execute real version and repository-status commands successfully, and all
  10 CTest targets pass with normal temporary-file access.
- Git leader-command labels now use their public dotted action identifiers
  (`git.status`, `git.refresh`, and related commands), matching the command popup
  convention used by configurable and third-party command registrations.
- File-row diff actions now preserve their source section: staged rows compare the
  index with `HEAD`, unstaged rows compare the worktree with the index, and
  untracked files render as additions against an empty file instead of showing
  "No diff is available".
- Fixed aliasing bug in `git_model_refresh`: when called with `task->snapshot.root`
  as both `root` and the snapshot pointer, `memset` zeroed the root string before
  copy, leaving `snapshot->root` empty and all diffs running from the wrong cwd.
- `git.status` (`L g g`) now toggles the panel; `git.diff`/`git.blame` show the
  panel on error so the message is visible; `git_relative_active_path` guards
  against an empty repo root.
- Explorer activation now explicitly selects the built-in file tree as the
  sidebar content, so `L e e` replaces an active Git or other contributed panel
  instead of treating any visible left panel as the explorer.
- The host owns a dynamic title-bar menu registry. Plugins can add items to
  `Plugins`, `View`, or any other existing menu, and can create a new top-level
  menu by supplying a new stable menu ID and label.
- Built-in menu layout keeps `View` focused on visible workspace surfaces,
  places editing/search actions under `Edit`, and groups plugin operations under
  `Plugins` submenus. Git contributes `View > Source Control` and `Plugins > Git`.
- The `Sol` menu owns New Buffer, Open File, and Open Folder first, followed by
  a separator and Settings. There is no separate `File` top-level menu.
- Causality menu overlays track hovered item indices explicitly and share
  viewport-aware dropdown/submenu geometry across paint, hit testing, clicks,
  and overlay suppression. This keeps highlights live and flips/clamps submenus
  that would otherwise render outside the window.

## Usage

- `Ctrl`, then `g g`: toggle Source Control panel.
- `Ctrl`, then `g r`: refresh.
- `Ctrl`, then `g d`: diff active file.
- `Ctrl`, then `g l`: history.
- `Ctrl`, then `g h`: branches.
- `Ctrl`, then `g b`: blame active file.
- `Ctrl`, then `g c`: focus commit input.
- `Ctrl`, then `g f/u/p`: fetch, pull, push.

## Verification Constraint

- Native launch and panel-open runtime paths completed without errors. macOS
  denied Accessibility keystroke injection and display capture, so pixel-level
  screenshot inspection was unavailable in this environment.
