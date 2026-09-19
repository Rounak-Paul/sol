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
