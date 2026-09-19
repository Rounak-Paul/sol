# Project tab bar / status bar consistency (2026-09-19)

## Request

Project tabs strip (top) and status bar (bottom) should look/feel the
same (same theme/style) — previously they used different background
colors. Initially also asked to add the same left/right gutter every
floating panel has (both bars were full-bleed/flush), but the user
reverted that specific part mid-session after seeing the reasoning:
**both bars should stay full width — that looks better** for a
window-spanning chrome bar. Only the color/theme-consistency half of
the request stands.

## Final fix

- `.status-bar`'s background changed from a standalone hardcoded
  `rgba(17, 17, 24, 0.88)` (in the earlier, non-winning rule block) to
  the same `rgba(5, 12, 21, SOL_UI_SURFACE_RAISED_ALPHA_CSS)` token
  `.project-tabs` already used, in the "Floating rounded glass
  composition" block that actually wins by cascade order.
- `sol_settings_build_appearance_css` (`sol_settings.c`): added
  `.project-tabs` to the shared `border-radius`/`backdrop-filter`/
  `opacity` rule that `.status-bar` was already part of — previously
  `.project-tabs` never got a live corner-radius/blur/opacity from the
  Settings appearance sliders while `.status-bar` did, so dragging those
  sliders visibly changed one bar but not the other.
- Both bars remain `width: 100%`, flush to the window edges — this was
  tried with an 8px gutter (matching floating panels) and explicitly
  reverted per user feedback. `SOL_UI_PANEL_MARGIN_PX_CSS` is still used
  for `.status-bar`'s top margin (the gap above it, unrelated to width).
- `style_retro.h`'s own `.project-tabs`/`.status-bar` overrides were
  touched then fully reverted back to original — Retro's bars were
  already flush/consistent with each other before this session; no net
  change there.

## Validation

- `cmake --build build -j 6`: clean.
- `ctest --test-dir build`: 20/20 passed (including `sol_style_tests`).
- `git diff --check`: clean. `style_retro.h` has zero net diff.
- Live launches under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`,
  both with the default Glass theme and (mid-investigation, before the
  revert) with Retro widget style active: zero stderr output each time,
  clean shutdown.
- Settings.json was snapshotted and restored exactly after each
  temporary widget-style switch used for live testing; testing only
  proceeded after confirming Sol was not already running.
- **Not yet visually confirmed by the user** —
  [[screencapture_unavailable]] still applies.

## Note for future sessions

Don't assume "inconsistency" complaints always mean "should match every
other panel's gutter convention" — the two chrome bars are a distinct
UI category (window-spanning strips) from floating content panels, and
the user confirmed full-width is the better fit for that category even
though it now differs from an 8px-inset floating panel. The fix that
stuck was purely about shared color/radius/blur tokens between the two
bars, not matching them to floating-panel geometry.

## Follow-up — Retro bevel parity + height/centering mismatch

Two more real inconsistencies surfaced after the above:

- **Retro widget style: bottom bar had no bevel.** `.project-tabs`
  (`style_retro.h`) got the full 4-sided raised-bevel treatment
  (`border-{top,left,bottom,right}-width: 1px` with light/dark per
  side), but `.status-bar` only set `border-top-width: 1px` (a single
  hairline, explicitly zeroing the other three sides) — so it read flat
  next to the tab strip's proper chiselled relief. Fixed by giving
  `.status-bar` the identical 4-sided `light/light/dark/dark` bevel
  pattern already used for `.project-tabs`/buttons/scrollbar-thumb, and
  updating `sol_retro_build_css`'s snprintf argument list (2 args →
  5 args at that position) to match the now-5-placeholder rule. Verified
  the argument-count change compiles without a format-string warning
  (a real risk when hand-editing a positional snprintf like this one —
  a silent mismatch would read garbage/out-of-bounds args).
- **Height mismatch was the real cause of "shifted"/"not centered"
  bottom bar content.** `.status-bar` had THREE places declaring
  `height: 22px` across the cascade (base rule, a "minimal glass theme
  overrides" rule, and no override in the final winning block — so 22px
  survived to the end), while `.project-tabs` is `19px` everywhere and
  `SOL_UI_STATUS_BAR_BAR_HEIGHT` (`sol_ui_internal.h`) — the constant
  the C-side reserved-band math is built from — is itself defined as
  `SOL_UI_PROJECT_TABS_HEIGHT` (19px). The reserved band assumed a 19px
  bar; the CSS actually rendered one 3px taller, which is exactly what
  produces vertically-off content and a bar that doesn't sit where its
  allotted band expects. Fixed both `22px` occurrences in `style.h` to
  `19px`. No C-side change needed — `SOL_UI_STATUS_BAR_BAR_HEIGHT` was
  already correct; only the CSS had drifted from it.
- Neither Retro nor the plugin theme (`sol-plugin-themes`) declares its
  own explicit height for either bar, so both now inherit the corrected
  19px from `style.h` uniformly across every active theme/style.

### Validation (follow-up)

- `cmake --build build -j 6`: clean, no format-string warnings from the
  Retro snprintf argument-count change.
- `ctest --test-dir build`: 20/20 passed.
- `git diff --check`: clean.
- Live-launch validation was skipped this round — the user had Sol
  already running (a live instance, ~1.5 min uptime) when this fix
  landed; didn't touch `~/.sol/settings.json` or start a second instance
  to avoid interfering with what they were actively looking at. Static
  verification (grep for every other hardcoded `22px`/height assumption
  in `workspace.c`/`status_bar.c`, confirming `.status-bar-badge`'s 14px
  and text's 12px both fit inside 19px with margin to spare) stands in
  for this round; **needs a restart + the user's next screenshot to
  confirm visually**, more so than usual since the running instance is
  still on the pre-fix binary.

## Round 2 — the actual deep bug: `height:100%` + `flex-grow:1` conflict

User's screenshot (first real visual evidence) showed the status bar
clipped with unrelated stdout log text ("[pipeline] SSBO layout
created...") visible below it, then separately reported "the panels
bottom bevel is not even showing." Both symptoms turned out to be the
SAME single root cause, confirmed via live instrumentation (temporary
`fprintf` diagnostics in `paint.c`, fully removed after — see method
below), not the height/bevel changes from Round 1 above (those were
real, separate, smaller fixes that still stand).

- **Root cause: `.workspace-main-full` (`style.h`) declared BOTH
  `height: 100%` and `flex-grow: 1`.** In this engine's flexbox layout
  (`layout.c`), a percentage height on a vertical flex child resolves
  as an explicit main-axis size (`ms = avail_main * height_pct`) BEFORE
  flex-grow distribution ever runs — explicit size always wins over
  flex-grow. `.workspace-main-full` sits inside `.workspace-host` (a
  vertical column) as the sibling AFTER the fixed-height `.project-tabs`
  strip. `height:100%` made it claim 100% of `.workspace-host`'s FULL
  height, completely ignoring that `.project-tabs` had already consumed
  space at the top — so it started correctly (after project-tabs) but
  ended `project_tabs_h` (~20.5px logical) past where `.workspace-host`
  actually ends. Since `.workspace-main-full`/`.workspace-main-content`
  both have `overflow: hidden`, this overflow amount became a hard clip
  boundary — but the actual content underneath (the split-tree panels:
  `.tree-panel`, `.buffer-pane`, `.term-panel`, `.plugin-side-panel`,
  `.welcome-pane`) laid out fine relative to their own (correct) parent
  chain, and ended up TALLER than their true visible bound. Their
  bottom border, and the last ~20px of their content, silently clipped
  away by the ancestor's own self-inflicted overflow.
- **Method — live instrumentation, not more guessing.** Added temporary
  `fprintf` diagnostics in `causality/src/ui/paint.c`'s `paint_border`
  (per this codebase's own established debugging convention — see
  [[floating-glass-ui-overhaul-2026-08-29]]'s many rounds): first logged
  a node's own resolved `border_top/right/bottom/left` width+color
  (confirmed CSS resolution was completely correct — all 4 sides had
  correct nonzero raised-bevel colors), then logged the incoming
  `clip` rect vs the node's own `y+h` (found a precise `gap=-11.88`
  logical px — the clip's bottom sat above the node's own bottom), then
  walked the full ancestor chain logging each node's `y`/`h`/`overflow_y`/
  `flex_grow` (found `.workspace-main-full`/`.workspace-main-content`
  both resolved to `662.76` — exactly `.workspace-host`'s OWN height,
  proving they were claiming space that included `.project-tabs`'s
  slice instead of the space actually remaining after it). Hand-computed
  the expected correct height two ways (`window_h - top - status_h` and
  cross-checked against Sol's own independent `sol_ui_buffer_area_rect_
  internal` C-side geometry helper, which had ALWAYS been computing the
  right number, `624.96` — this parallel Sol-side geometry math was
  never the problem, confirming the bug was purely in the CSS/flex
  layer) before making the one-line fix.
- **Fix:** removed the conflicting `height: 100%;` from
  `.workspace-main-full` in `style.h` — `flex-grow: 1` alone already
  correctly fills the remaining space after `.project-tabs`. Re-ran the
  same diagnostic after the fix: `gap=0.000` exactly,
  `.workspace-main-full`/`.workspace-main-content` now resolve to
  `642.24`/`624.96` matching Sol's own hand-computed/C-side expectation
  precisely. All temporary diagnostics fully removed — confirmed via
  `git diff --stat vendors/causality/` showing zero diff on the
  submodule (the entire fix lives in Sol's own `style.h`, one CSS
  property removed).
- **Blast radius:** this affected EVERY panel inside the main workspace
  splitter simultaneously — tree panel, buffer pane, terminal panel,
  plugin side panel, and the welcome pane all silently lost their
  bottom ~20px (bottom border/bevel invisible, last content row
  potentially clipped) whenever `.project-tabs` was present (i.e.
  whenever a project tab strip is shown at all, which is normal/default
  operation). This was probably present long before this session's
  other bar-consistency work and is unrelated to any of the earlier
  Round 1 changes — it's a pre-existing CSS bug this investigation
  happened to surface.
- Build clean, 20/20 CTest, `git diff --check` clean, live launch under
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` zero stderr output.
  Net diff: `sol/src/ui/style.h`, 11 insertions / 1 deletion (mostly the
  explanatory comment; the actual fix is a single removed CSS line).
- **Not yet visually confirmed by the user** — needs a fresh relaunch
  and screenshot; the numeric proof (gap=0.000, matching Sol's own
  independent geometry helper exactly) is about as strong as static+
  runtime verification gets without an actual frame capture, but this
  codebase's own history has examples of "numerically correct but still
  visually wrong for an unrelated reason" (Round 5 of the floating-glass
  file), so treat this as high-confidence, not proven-on-screen.
