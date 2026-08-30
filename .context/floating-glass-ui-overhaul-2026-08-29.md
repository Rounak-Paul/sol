# Floating glass UI overhaul

## Goal

Turn the main Sol workspace into a floating glass composition: side panels,
editor leaves, terminals, and transient cards sit inside rounded clipped
surfaces with consistent outer gutters and draggable split gaps. The animated
theme canvas remains sharp and visible between panels, while blur is restricted
to each rounded panel footprint.

## Ownership

- `sol/src/ui/style.h` owns structural spacing, clipping, radii, and interaction
  styling shared by the Glass fallback and every derived plugin theme.
- `sol/src/ui/workspace.c` owns top-level panel layout, panel-handle tracking,
  terminal geometry, and conversion of resolved Causality rectangles into
  background blur regions.
- `sol/src/core/sol_bg_effect.c` owns the GPU blur mask. Blur regions carry a
  normalized corner radius and the vertical blur pass clips fragments to that
  rounded rectangle.
- Bundled Causality owns CSS `backdrop-filter` composition. Its image pipeline
  must apply the draw command's per-corner radii when sampling blurred content.
- `sol/src/core/sol_buffer.c` owns split-tree geometry. The caller supplies the
  rendered split-bar size so render metrics and hit geometry match Causality.
- `sol/src/core/sol_settings.c` owns the user appearance overlay and must apply
  the configured radius/blur/opacity to every panel and interactive surface.

## Geometry contract

- Workspace outer gutter: 8 logical px.
- Every draggable divider: 8 logical px, acting as both the resize target and
  the visible canvas gap.
- Default panel/card radius: 12 logical px; interactive controls use a smaller
  derived radius through the appearance overlay.
- Buffer geometry subtracts the outer gutter, left-panel divider, terminal
  divider, and every nested editor divider using the same scaled constants as
  the rendered layout.
- Blur rectangles are read from resolved `Ca_Div` screen rectangles between
  layout and paint. They are not inferred from panel ratios.

## Validation boundary

- Build all targets and run the full CTest suite.
- Run `git diff --check` for Sol and the bundled Causality worktree.
- Launch the native app with Vulkan validation and inspect the live window when
  host capture is available. Build/test success alone is not visual proof.

## Progress

- Confirmed the previous workspace and blur geometry were edge-to-edge.
- Confirmed the late Glass stylesheet and appearance defaults forced most
  controls to square corners.
- Confirmed split traversal hard-coded a 1 px divider and omitted terminal
  subdivision from buffer render geometry.
- Added 8 px outer workspace padding and used the same 8 px value for side,
  terminal, and nested editor splitters. The transparent splitter remains the
  draggable hit target while exposing the canvas.
- Tracked resolved side/editor/terminal/status/command-card handles and now
  build localized blur regions from `ca_div_screen_rect()` between layout and
  paint. Region updates are change-detected to avoid invalidation loops.
- Extended background blur regions with normalized corner radii and added a
  rounded-rectangle coverage mask to the vertical GPU blur pass. This preserves
  sharp animated background in gutters and antialiased panel cutouts at HiDPI.
- Rounded and clipped primary surfaces, transient cards, tabs, rows, buttons,
  inputs, scrollbar thumbs, and source-control actions. Appearance settings use
  the full configured radius for panels and a capped half-radius for controls.
- Fresh-install appearance defaults are now 12 px radius, 16 px panel backdrop
  blur and 12 px title-bar blur. Scrollbars are always pill-shaped from their
  configured width; the redundant radius preference has been removed.
- Removed the buffer traversal's private 1 px divider assumption: callers now
  pass the rendered pixel size. Updated render, input, scrolling, viewport, and
  tab geometry to honor both the shared gap and UI scale.
- Buffer root geometry now subtracts the active terminal split, fixing the
  pre-existing visible-line and hit-geometry mismatch when the terminal is open.
- Removed full-height panel shadows after visual inspection showed their broad
  rectangular kernels muddying the canvas gaps. Rounded backdrop quads now clip
  in Causality's image shader instead of compositing rectangular silhouettes.
- Reserved a full 30 px status strip for a centered 22 px pill with 8 px side
  and bottom gutters. The prior 18 px pill was forced into a 22 px strip and
  could not satisfy its own margins.
- Capped background-effect blur passes at four for chrome and two for editor
  content. Out-of-range persisted values fall back to the established 3/1 pass
  defaults, preventing high-pass smearing while preserving explicit valid values.
- Replaced the localized blur mask's binary rounded discard with antialiased
  alpha coverage and enabled blending only for the vertical composite pass.
  This removes the hard blur boundary visible beneath translucent panel edges.
- Propagated the configured panel radius into the buffer tab strip, body,
  scroll row, and gutter edge. Open editor children can no longer repaint the
  pane's rounded top or bottom corners with square surfaces.
- Removed two leftover `fprintf(stderr, ...)` debug probes from the prior
  session (`sol_bg_effect.c`'s per-region blur logger, capped at 10 lines) —
  confirmed via live runs that the blur-region math (screen-rect fractions,
  corner-radius reconstruction against the physical swapchain extent, and the
  `ui_scale` application) is internally consistent end-to-end, including
  across the logical/physical DPI boundary. Not the source of the reported
  artifact.
- Root-caused the reported "sharp silhouette behind rounded corners" as CSS
  coverage gaps, not a rendering-math bug: `.scm-tab`/`.scm-tab-active`
  (source-control tab strip) and `.pm-badge` (plugin-manager status pill)
  were the only two interactive/card elements never re-rounded by the
  appearance-overlay pass in `sol_settings_build_appearance_css` (the CSS
  that actually wins at runtime) or the static fallback theme in `style.h`'s
  "Floating rounded glass composition" section — every other `corner-radius:
  0px` base-theme rule was confirmed covered by cross-referencing against
  both override blocks. Added `.scm-tab`/`.scm-tab-active` to the 6px group
  (matching `.buffer-tab`) and `.pm-badge` to a 7px rule (matching
  `.status-bar-badge`) in both `style.h` and `sol_settings.c`.
- Verified fix does not overflow the 4096-byte appearance-overlay buffer
  (measured 2065 bytes live, well under budget).

## Round 2 — actual root cause, found via user screenshot

The `.scm-tab`/`.pm-badge` CSS gap above was real but NOT the reported bug —
user confirmed via a fresh screenshot the artifact was unchanged/worse.
Two further causes were found and fixed:

- **H-pass full-screen blur bleed.** `blur_execute`'s horizontal Gaussian
  pass blurred the ENTIRE swapchain every iteration with no region
  restriction, while the vertical pass was already correctly scissored +
  SDF-masked per rounded panel. On multi-pass blur (this session's live
  settings request 4 passes), pass N's H-blur re-samples pass N-1's
  composited swapchain — which has a hard, unblurred edge exactly at each
  panel's rounded-corner SDF cutoff (V-pass `discard`s outside it,
  preserving crisp background) — and smears that hard edge sideways into
  scratch, which the next V-pass then re-composites, producing a visible
  rectangular fringe hugging the curve. Fixed by scoping the H-pass to each
  active region's footprint (+ kernel sample-radius padding), never the
  full screen (`sol_bg_effect.c`, `blur_execute`).
- **No rounded clip in the engine (the real root cause).** Causality's
  `overflow:hidden` clipping was, and had always been, a plain axis-aligned
  hardware scissor (`ClipRect` in `paint.c` had no radius field; `Ca_DrawCmd`
  clip fields were x/y/w/h only). A panel's OWN background rect painted with
  a rounded SDF, but any child content near its edge (a hovered/selected
  row, a scrollbar track, anything whose fill differs from the panel
  background) clipped to the panel's square bounding box and rendered
  square right up to it — visibly poking past the rounded corner. This is
  independent of blur entirely and was the actual artifact in both
  screenshots. Fixed at the engine level (not a Sol-only patch):
  - `ClipRect` (paint.c) gained a `radius` field; `clip_intersect` takes a
    `new_radius` param and only keeps it when the new ancestor's edge is
    still the tightest bound on every side (a square-cornered parent clip
    that shrinks the rect must not inherit a child's radius on a corner it
    no longer owns).
  - `find_clip_for_node` and the child-recursion clip in `paint_tree_cached`
    now pass `cur->desc.corner_radius` / `node->desc.corner_radius` through.
  - `Ca_DrawCmd` gained `clip_radius`; `set_clip` copies it through.
  - `Ca_ClipPushConst` (ca_internal.h, 24 bytes: vec2 pos, vec2 size, float
    radius, pad) is pushed to the **fragment stage only** of the rect
    pipeline, updated whenever swapchain.c's rect-batch scissor OR clip
    shape changes (batches are grouped by scissor equality already, so one
    push constant per batch is sufficient — no per-instance SSBO growth,
    which matters because `Ca_RectPushConst`/`Ca_TextInstance` share a
    hard-fixed 128-byte instance stride with zero spare bytes).
  - Rect vertex shader gained a `v_node_pos` varying (absolute node-space
    pixel position, pre-NDC) so the fragment shader can evaluate the clip's
    rounded-box SDF without knowing the clip's position relative to each
    instance. Fragment shader discards when `clip_radius > 0` and the
    fragment is outside the rounded clip SDF (hardware scissor still does
    the cheap bounding-box pre-cull; the SDF only refines the corners).
  - **Scope: rect draw path only** (text/glyph, image, viewport, and
    backdrop-blur draw paths still clip to a plain rectangle). Chosen
    deliberately — the reported artifact is background/row fills, not text
    or images reaching exactly into a rounded corner. Revisit if a future
    report shows glyphs/images doing the same.
- Both fixes: build + full CTest suite pass, `git diff --check` clean on Sol
  and the Causality submodule, stable launch with zero log output under
  plain run AND `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` (no VUID
  errors — relevant here since this touched a pipeline layout's push-constant
  range, a common validation-error source). Runtime GLSL shader compilation
  (rect vert+frag) succeeds — confirmed via clean stderr on a live run, not
  just a C build pass, since shader source is compiled at startup, not by
  the C compiler.
- Still no pixel-level visual confirmation possible from this session
  (screencapture permission unavailable — see [[screencapture_unavailable]]).
  User is verifying via manual screenshot after each round.

## Round 3 — scrollbar track/thumb missed by the rounded clip

Round 2's engine-level rounded-clip fix made things visibly worse: user's
next screenshot showed a dark vertical seam running the full height of the
tree panel's right edge that wasn't as prominent before.

- **Root cause: `paint_scrollbars` never picked up the new radius.**
  `paint_tree_cached` computes two things from a node's own `overflow:hidden`
  bounds — `child_clip` (narrowed to `ca_scrollbar_viewport_*`, i.e.
  excluding the scrollbar gutter, used for recursing into children) — but
  the post-children scrollbar-painting call `paint_scrollbars(win, node,
  clip)` was passed the node's *incoming, un-narrowed* `clip` from its
  parent, which for a top-level rounded panel like the tree panel has
  radius 0 (the ancestor above it is the unrounded workspace root). Every
  other child (rows, etc.) now correctly inherits `child_clip`'s radius
  from round 2's fix, so the scrollbar track/thumb — positioned flush
  against the panel's own right edge, spanning its full height — was the
  one remaining element still rendering to a square boundary, and now
  stood out by contrast against everything else that had become properly
  rounded. This gap almost certainly predates this session's work entirely
  (the scrollbar's clip parameter was never `child_clip`-narrowed even
  before rounded clipping existed) — it just weren't visible until its
  neighbors stopped sharing the same square-corner behavior.
- **Fix:** added a third clip variant, `own_clip` — the panel's *own full
  bounds* (`node->x/y/w/h`, not narrowed to exclude the scrollbar gutter
  like `child_clip`) intersected with the incoming `clip`, carrying the
  same `node->desc.corner_radius`. `paint_scrollbars` now receives
  `own_clip` instead of the raw incoming `clip`, so its four rects
  (V-track, V-thumb, H-track, H-thumb — all four already funnel through
  the same `clip` parameter) are masked to the panel's rounded shape
  without being narrowed away by the viewport-exclusion `child_clip` would
  have applied (which would have clipped the thumb itself out of existence,
  since it deliberately sits in the excluded gutter).
- Build + full CTest suite pass again; stable launch with zero log output
  under both plain and `VK_LAYER_KHRONOS_validation` runs.
- User's next screenshot (readable directly this time, not via the temp
  path) confirmed the vertical scrollbar seam was gone — that fix held. But
  it surfaced a new symptom: dark horizontal banding at the top/bottom edge
  of every rounded panel, plus lingering darker patches right at corners.

## Round 4 — blur H-pass padding was one-axis only

- **Root cause:** round 2's H-pass scoping fix (see above) padded the
  per-region scissor horizontally (`h_pad`) so the H-pass's own horizontal
  taps stay fed, but left the vertical bounds unpadded. The very next
  V-pass samples **vertically** from those same scratch rows, reaching ±4
  texels above/below the region's top and bottom edge on its 9-tap kernel —
  exactly the margin the H-pass never wrote to (scratch attachment uses
  `LOAD_OP_DONT_CARE`, so unwritten texels there are undefined garbage on
  every pass after the first). The V-pass blended that garbage into the
  region's top/bottom rows, reading as a dark horizontal band; the same
  effect compounding at all four corners (where both axes' padding
  deficiency overlap) produced the darker corner patches the user flagged
  separately.
- **Fix:** renamed `h_pad` to `sample_pad` and applied it to the H-pass
  scissor on BOTH axes (x and y), not just x. One-line semantic change,
  same kernel-reach constant already computed correctly
  (`4 * SOL_BG_EFFECT_BLUR_SAMPLE_SPREAD`, rounded up) — it was only ever
  applied to half the axes it needed to be.
- Build + full CTest suite pass; stable launch with zero log output.
- Round 4's fix (banding) was never independently confirmed — round 5
  found the actual root cause underneath everything, which likely explains
  every screenshot since the very first one in this session.

## Round 5 — THE actual root cause: `corner-radius` is not a real CSS property

User, correctly frustrated after four rounds of plausible-but-wrong fixes,
said the picture was already clear and to stop asking for more screenshots.
Re-read the existing screenshots literally instead: a panel's OWN
background was rendering perfectly square while a separately-rounded blur
mask sat on top of it — "rectangular thing behind the rounded corner" is
exactly what that looks like, and no amount of blur-pass or clip-engine
work could ever fix a square background rect.

- **Root cause:** Causality's CSS parser (`vendors/causality/.../ui/css.c`)
  only ever registered `"border-radius"` (plus the four longhand
  `border-{top,bottom}-{left,right}-radius`) in its property table
  (`css.c:790-794`) and its shorthand parser (`css.c:1852`,
  `strcasecmp(prop_name, "border-radius")`). **`corner-radius` was never a
  recognized property — anywhere, ever.** Every `corner-radius: Npx;`
  declaration in the entire Sol UI layer was silently parsed as an unknown
  property and dropped: 52 occurrences in `style.h` (including the single
  most important rule, `.tree-panel, .plugin-side-panel, .buffer-pane,
  .term-panel, .welcome-pane { corner-radius: 12px; ... }` — the one rule
  meant to round every floating panel's own background) and all 5
  occurrences in `sol_settings.c`'s `sol_settings_build_appearance_css`
  (meaning the Settings window's "Corner Radius" slider has never visibly
  affected a single panel background in this app's history — it only
  happened to affect the *blur mask*, which is driven by C struct fields,
  not CSS, and was never broken).
  - This makes total sense of every prior screenshot: panels/rows/buttons
    that got their rounding from actual `border-radius:` rules (scattered
    correctly through the base Glass theme for buttons, tabs, etc. — see
    the working uses at e.g. `.welcome-btn`/`.welcome-btn-primary`) *did*
    round correctly and consistently across every screenshot. The main
    panel containers (`welcome-pane`, `tree-panel`, `buffer-pane`,
    `term-panel`) never did, because their sole rounding rule used the
    fake property. What LOOKED rounded about them was purely the
    (correctly rounded, C-driven) background-blur mask sitting on top —
    the panel's own square fill showed through at every corner, at every
    round of "fixes," because none of those fixes touched CSS property
    recognition.
  - Round 1's `.scm-tab`/`.pm-badge` CSS gap fix, while directionally
    sensible, used the same broken `corner-radius:` property and was
    therefore *also* a no-op the whole time — it's now fixed for real as a
    side effect of this round's blanket replace.
- **Fix:** `sed -i '' 's/corner-radius:/border-radius:/g'` across
  `sol/src/ui/style.h` (52 replacements) and manual replacement of the 4
  `corner-radius:` occurrences in `sol_settings.c`'s generated CSS string
  plus its doc comment. No selector, value, or specificity changed — only
  the property keyword, from fake to real.
- Build + full CTest suite pass; `git diff --check` clean on Sol and the
  Causality submodule; stable launch with zero log output.
- Rounds 2–4's blur-pass and clip-engine fixes (H-pass full-screen bleed,
  scrollbar own-clip, H-pass two-axis padding, and the engine-level rounded
  ClipRect/push-constant work) are NOT wasted — they were real, separate
  bugs the investigation surfaced along the way and remain correct/needed.
  But none of them could have fixed the user's actual complaint on their
  own, because the panel background itself was never getting a radius from
  CSS at all until this round.
- **Confirmed fixed by the user** — round 5's `corner-radius` → `border-radius`
  property-name fix was the real root cause of the corner artifact across
  every screenshot in this session. Closed.

## Round 6 — design consistency pass (not a bug fix; a deliberate scale pass)

With the corner bug closed, user asked for a broader consistency pass:
inconsistent rounding scale across the whole UI, buffer tabs vs terminal
tabs mismatched height/color, inconsistent scrollbar styling, the
command-flow popup's layout/margin, and "rounding is wayyy too much
overall, tone it down."

- **Chosen scale** (user picked "tight": 4 / 8 / 10px over the softer 6/10/14
  alternative): `SOL_UI_CONTROL_RADIUS_PX` = 4px (tabs, badges, buttons,
  inputs, scrollbar thumbs, popup rows), `SOL_UI_PANEL_RADIUS_PX` = 8px
  (floating panels, popups/menus/cards — was 12px), `SOL_UI_PILL_RADIUS_PX_CSS`
  = 10px (status bar, the one full-height rounded strip). New constants
  added to `sol_ui_constants.h`; the "Floating rounded glass composition"
  block in `style.h` rewritten to reference them instead of the previous
  ungoverned spread of 0/2/5/6/7/10/11/12px hardcoded per-selector.
  `SOL_SETTINGS_CORNER_RADIUS_DEFAULT` (sol_settings.h) changed 12→8 to
  match — the live appearance-overlay slider derives `control_radius =
  min(cr*0.5, 10)`, so `cr=8` lands exactly on control=4/panel=8, keeping
  the slider-driven and hardcoded-default paths on the same scale.
- **This user's saved `~/.sol/settings.json` had `corner_radius: 20`** (the
  old slider max, saved while chasing the original bug pre-session) — reset
  to `8.00` directly in the file so the new scale actually takes effect for
  them without waiting on a manual slider re-drag. Left `panel_blur:0,
  titlebar_blur:0, panel_opacity:0.33, scrollbar_width:10.09` untouched —
  not in scope for this ask. (`scrollbar_radius:1.09` in that same file is
  dead data — `SolSettings`'s JSON parser has no such field; the real
  scrollbar radius is always derived live as `scrollbar_width * 0.5`.)
- **Buffer tab vs terminal tab parity** — `.buffer-tabs-row`/`.buffer-tab`
  use a "22px strip holding an inset 20px pill tab" pattern; `.term-header`/
  `.term-tab` used a completely different "28px tab flush to its own 28px
  row" pattern with asymmetric `12px` left padding (vs buffer-tab's `7px`)
  and solid opaque colors (`#18181c`/`#0e0e10`) instead of the translucent
  `rgba(...,0.82)` used everywhere else in this "floating glass" design.
  Left `.term-header`/`.term-tab` at their original 28px height (changing
  terminal row height has knock-on layout effects in `sol_buffer.c`'s
  split-geometry math that were out of scope here) but: gave `.term-header`
  the buffer-tabs-row's rounded top corners + 1px/3px padding rhythm
  instead of flush corners; shrank `.term-tab`/`.term-tab-active` to 20px
  (inset pill, matching `.buffer-tab`'s height inside its row) with
  matching 7px/2px padding; and swapped both tab colors and the header
  background to the same translucent `rgba(...,0.82)` values `.buffer-tab`/
  `.buffer-tabs-row` already use, so terminal tabs finally read as the same
  design language as buffer tabs, not a visually distinct subsystem.
- **Command-flow popup margin** — `.cf-overlay`'s hardcoded `10px` bottom/
  right padding replaced with `SOL_UI_PANEL_MARGIN_PX_CSS` (8px), matching
  every other floating panel's outer gutter instead of a one-off value.
  Investigated the "gets cut weirdly" report in `command_panel.c`'s
  `render_suggestion_row`: the "More" label repeated on every row is
  intentional documented fallback text (`command_flow.c:111-140`, used
  when a which-key continuation node has no explicit label — not a bug),
  and the row's flex layout (key chip fixed-width, label flex-grow:1, "+N"
  badge right-aligned) is structurally sound. No further code-level cause
  found for a "cut" look beyond the margin fix above; flagged as needing a
  screenshot to pin down further if it's still off after this round — this
  one specific item, not the whole pass, since the user said not to ask
  for screenshots as a blanket rule but this is a narrow, isolated follow-up.
- Scrollbar consistency: the tree panel's native scrollbar (wildcard `*`
  selector) and the buffer editor's custom scrollbar already shared the
  same live `scrollbar_width`/`scrollbar_radius` derivation via the
  appearance overlay before this round — verified, not changed. The
  visible inconsistency the user saw was most likely the base-theme
  defaults (8px square wildcard vs 9px rounded buffer scrollbar) showing
  through on panels/controls that hadn't picked up the overlay for some
  other reason (e.g. before settings load) rather than a live divergence;
  no code change made here since the live path was already correct.
- Build + full CTest suite pass; `git diff --check` clean; stable launch
  with zero log output. Touched files this round: `sol_ui_constants.h`,
  `sol_settings.h`, `style.h`, plus the user's `~/.sol/settings.json`.
- **Not yet visually confirmed** for this round specifically (user asked to
  stop requesting screenshots as a default, so wait for the user to bring
  the next one rather than asking).

## Round 7 — cross-window opacity consistency pass (heavy UI/UX polish)

User asked for a "heavy UI/UX pass and polish" covering every window and
panel, not just the main workspace, with glass background styling kept —
explicitly to make every window/panel "feel the same."

- **Confirmed the active theme system (`sol-plugin-themes`'s
  `build_theme_css`) already had a clean 4-tier semantic opacity model**
  (`chrome`/`panel`/`editor`/`raised` tokens at fixed alpha per
  light/dark) applied uniformly across every window including the four
  auxiliary dialog windows. This is the path most users see day-to-day —
  it was not the problem.
- **Found two real, narrow gaps in the Glass fallback theme
  (`style.h`, used pre-plugin-load or when explicitly selected):**
  root/header background alpha values were ad-hoc per-window (0.72–0.92,
  ~15 distinct values with no shared token) instead of following the same
  tiered model the plugin themes use.
- **Found one real gap in the live appearance overlay
  (`sol_settings_build_appearance_css`, `sol_settings.c`):** the
  Settings > Theme picker's panel-opacity/blur slider was applied to
  `.tree-panel, .plugin-side-panel, .buffer-pane, .term-panel,
  .welcome-pane, .cf-panel, .status-bar` but never to the four dialog
  window roots (`fp-root`/`search-root-window`/`pm-root`/`sw-root`) —
  dragging the slider visibly changed the main workspace but did nothing
  to File Picker, Search, Plugin Manager, or Settings' own window.
- **Ruled out `scm-root`:** dead selector, no producer anywhere in the
  codebase (git plugin renders `scm-view` inside `plugin-side-panel`,
  which already inherits blur/opacity from its ancestor). No change
  needed.
- **Ruled out adding `border-radius` to the four dialog-window roots:**
  confirmed via `ca_window_create` call sites that Settings/Plugin
  Manager/File Picker/Search are plain OS-decorated square windows (no
  transparency/borderless flags), unlike in-canvas floating panels.
  Rounding a root div that exactly fills a square window's client area
  would expose the window's default backdrop at the corners, not clip
  cleanly — opacity/blur only for these four.
- **Ruled out `backdrop-filter` as risky:** cross-checked against
  [[backdrop_blur_removed]] — that abandonment was specific to
  menu-popup blur (`.ca-select-popup`/`.ca-tooltip`/`.ca-context-menu`/
  `.ca-menubar-popup`, GPU `image_pipeline` producing zero pixels).
  Panel-level `backdrop-filter` (the exact mechanism this round extends
  to dialog roots) is the proven-working path this whole 6-round session
  built and validated — safe to reuse.
- **Fix 1:** added three CSS-string constants to `sol_ui_constants.h`
  (`SOL_UI_SURFACE_CHROME_ALPHA_CSS "0.86"`, `..._RAISED_ALPHA_CSS
  "0.90"`, `..._WELL_ALPHA_CSS "0.86"`) mirroring the plugin theme's
  chrome/raised/well surface roles, and spliced them into every
  background-alpha declaration in style.h's "Minimal glass theme
  overrides" section (tree/plugin-side-panel, buffer-body, status-bar,
  cf-panel, fp-root/search-root-window, fp-toolbar/search-header/footer,
  fp-list/search-results, fp-new-folder-input/search-input, welcome-pane,
  pm-root/sw-root, pm-left/sw-left, pm-search-row,
  pm-search-input/sw-scale-input/sw-select, scm-root/scm-view,
  scm-toolbar/repository/commit-box/section-header,
  scm-commit-input/branch-input). Colors (rgb hue) were left untouched —
  only the alpha channel was unified, preserving each window's
  intentional tint.
- **Fix 2:** added `.fp-root, .search-root-window, .pm-root, .sw-root {
  backdrop-filter: blur(%.1fpx); opacity: %.3f; }` (no border-radius) to
  `sol_settings_build_appearance_css`, reusing the same `pblur`/`op`
  values as the main-panel rule, with an updated `snprintf` arg list.
- Build + full CTest suite (14/14) pass; `git diff --check` clean on Sol
  and the Causality submodule; stable launch with zero log output under
  both plain and `VK_LAYER_KHRONOS_validation` runs.
- Appearance-overlay buffer stays well under its 4096-byte cap (prior
  measured baseline 2065 bytes + ~102-byte new selector block ≈ 2167).
- **Not yet visually confirmed** — screencapture still unavailable this
  session (see [[screencapture_unavailable]]). This round is lower-risk
  than rounds 1–5 (pure alpha-value/selector-coverage edits, no new
  rendering-engine code paths), but still needs the user's own screenshot
  to close out.

## Round 8 — focused-panel visual indicator

User: "the currently focused panel needs some visual indicator." Before
this round there was zero cross-panel visual distinction for keyboard
focus: `.buffer-pane-active` differed from `.buffer-pane` only in an
alpha the "Minimal glass theme overrides" block had already zeroed to
`transparent`, and the tree panel / terminal had no focus concept at all
in the CSS layer (tree-panel click routing existed via
`focus_region_callback`'s `in_explorer` bool and `main.c`'s
`app->explorer_focused`, but neither drove anything visible).

- **Confirmed all three keyboard-focus transfer points already existed**
  as separate, uncoordinated mechanisms: `app->explorer_focused`
  (main.c, tree-panel), `sol_terminal_manager_focused()` (terminal), and
  `active_leaf_id` + `buffer_input_active` (buffer leaves). No unified
  concept lived in `SolUISystem` for "which top-level panel has focus" —
  that's the actual gap; the individual signals were all fine.
- **Fix:** added `SolUIFocusedPanel` enum (`sol_ui_system.h`:
  BUFFER/TREE/TERMINAL) plus `sig_focused_panel` (u32 signal, mirrors the
  existing `sig_file_tree_visible` value-signal pattern — not a revision
  counter) and a plain `focused_panel` field on `SolUISystem`, with
  `sol_ui_system_set_focused_panel()` (change-guarded, matches
  `sol_ui_system_set_file_tree_visible`'s idiom) /
  `sol_ui_system_focused_panel()` getter pair.
- **Wired at every real focus-transfer site**, not a new poll: pane
  click, tab click, `sol_ui_system_focus_leaf` → BUFFER; file-tree
  `on_row_click` (file_tree_panel.c) and the keyboard-driven
  `explorer.focus.toggle` path (`sol_toggle_explorer_focus` in main.c,
  entering-explorer branch only — the leaving-explorer branch already
  calls `sol_ui_system_focus_leaf` internally, covered for free) → TREE;
  `sol_ui_system_terminal_set_focused(ui, true)` → TERMINAL. Did NOT add
  a blind reset in main.c's "all non-explorer actions dismiss explorer
  focus" block — that fires on every command dispatch regardless of
  whether a buffer is actually touched, so it would have produced false
  BUFFER flashes for unrelated commands (e.g. opening Settings).
- **Style layering, not a replacement:** the active split leaf still
  gets `.buffer-pane-active` regardless of keyboard focus (that's
  "which pane you'd type into if you clicked back into the buffer
  area", a split-tree concept); `.buffer-pane-focused` is layered on
  top only when `is_active && focused_panel == BUFFER`, which is the
  narrower "your keystrokes go here right now" claim. Tree/plugin-panel
  and terminal roots get a plain `-focused` modifier class with no
  competing "-active" concept to layer against.
- **Visual: soft accent-blue glow via `shadow-*`, not a border.**
  Checked `style.h` first — zero border usage anywhere in this design
  system; every existing affordance is background-alpha or shadow-based
  (`.cf-panel`'s drop shadow is the only prior `shadow-*` user). A hard
  border would have broken the established "glass, no borders" language.
  **Checked the engine before relying on `box-shadow` spread** (echoing
  the `corner-radius` trap from Round 5): `causality/src/ui/css.c`'s
  box-shadow shorthand parser explicitly discards spread
  (`nums_seen < 3` only captures offset-x/offset-y/blur; a 4th number
  falls into the catch-all `parser_next` and is dropped) — so a crisp
  ring is not achievable, only offset+blur+color. Used the longhand
  `shadow-offset-x/y: 0px`, `shadow-blur`, `shadow-color` properties
  directly (matching `.cf-panel`'s existing style) for a centered glow.
- **Checked shadow clip risk before picking the blur radius.**
  `paint_node_content` (causality's paint.c) clips the shadow draw
  command to the *incoming* parent clip, not the node's own
  `overflow:hidden` bound — but `.workspace-main-content` (the direct
  ancestor of every panel root) is itself `overflow:hidden` with only
  `SOL_UI_PANEL_MARGIN_PX` (8px) padding. A wider glow would be clipped
  asymmetrically at the window/split edges. Capped blur at 7px (under
  the 8px gutter) and raised alpha to 0.65 to keep the glow legible at
  that radius. New constants: `SOL_UI_FOCUS_GLOW_COLOR_CSS`,
  `SOL_UI_FOCUS_GLOW_BLUR_PX_CSS` (`sol_ui_constants.h`).
- **CSS rule added to style.h's "Floating rounded glass composition"
  section** (the block that wins by cascade order across every active
  theme — confirmed neither `sol-plugin-themes`' `build_theme_css` nor
  `sol_settings_build_appearance_css` ever declare `shadow-*` on these
  selectors, so this rule is theme-independent and always wins):
  `.tree-panel-focused, .plugin-side-panel-focused,
  .buffer-pane-focused, .term-panel-focused { shadow-offset-x: 0px;
  shadow-offset-y: 0px; shadow-blur: 7px; shadow-color:
  rgba(91,151,218,0.65); }`.
- Build + full CTest suite (14/14) pass; `git diff --check` clean on Sol
  and the Causality submodule; stable launch with zero log output under
  both plain and `VK_LAYER_KHRONOS_validation` runs.
- **User feedback after trying it: "becomes blue colored all with a
  sharp rectangle, very weird."** Root-caused via the fragment shader
  (`causality/src/renderer/pipeline.c`'s embedded GLSL, `shadowAlpha`):
  `exp(-max(d,0)²/2σ²)` only decays for `d > 0` (outside the rounded-box
  SDF); for every fragment *inside* the shape `d < 0` so `max(d,0) = 0`
  and the exponential is `1.0` — full solid opacity everywhere inside
  the box. A zero-offset "glow" shadow is therefore not a glow at all,
  it's a solid fill; the intended soft OUTER fringe was also being
  clipped by `.workspace-main-content`'s `overflow:hidden` 8px gutter
  (per Round 8's own risk note), so only the solid inner rectangle
  survived — exactly the reported artifact. This shadow mode is built
  for drop shadows sitting behind an opaque panel (which paints over the
  solid inner fill), not a standalone ring on a translucent glass panel.
  **Round 8's shadow-based approach is invalid for this use case** —
  not a tuning issue, a wrong primitive.
- **Fix: switched to a real border.** Checked the border render path in
  the same shader first (`bm = aa_outer - aa_inner`, an inset annular
  band using shrunk-by-border-width inner SDF radii) — correctly
  contained within the node's own bounds, so immune to both the
  inside-fill bug and the parent-gutter clip risk. `border-width` /
  `border-color` are real recognized CSS properties (confirmed against
  `causality/src/ui/css.c`'s property table, unlike the `corner-radius`
  trap from Round 5) already used elsewhere in this engine (title-bar
  buttons, editor's border list). Replaced the `SOL_UI_FOCUS_GLOW_*`
  constants with `SOL_UI_FOCUS_BORDER_WIDTH_PX_CSS` ("1.5px") /
  `SOL_UI_FOCUS_BORDER_COLOR_CSS` ("rgba(106,163,227,0.85)") in
  `sol_ui_constants.h`, and the `.tree-panel-focused,
  .plugin-side-panel-focused, .buffer-pane-focused, .term-panel-focused`
  rule in style.h now sets `border-width`/`border-color` instead of
  `shadow-*`. Border alpha is independent of the panel's `opacity` CSS
  property (paint.c only multiplies fill alpha by node opacity, not
  border alpha) — stays legible regardless of the user's panel-opacity
  slider setting.
- Build + full CTest suite (14/14) pass; `git diff --check` clean on Sol
  and the Causality submodule; stable launch with zero log output under
  both plain and `VK_LAYER_KHRONOS_validation` runs.
- User confirmed via screenshot: border still rendered as "the panel
  becomes blue colored all with a sharp rectangle" — a SOLID fill, not a
  ring, at 1.5px. Round 8's border switch was directionally right but had
  its own engine-level bug.

## Round 9 — two real engine bugs found via user screenshots + numeric diagnostics

Both root-caused in the shared Causality rect pipeline (`vendors/causality`),
not Sol-only — found via targeted `fprintf` diagnostics on live runs plus a
standalone C reimplementation of the fragment shader's SDF math to check it
numerically, since screencapture is still unavailable this session (see
[[screencapture_unavailable]]) and this needed more than eyeballing GLSL text.

- **Bug 1 — border/fill composite wasn't premultiplied.**
  `causality/src/renderer/pipeline.c`'s FRAG_GLSL border path computed
  `out_color = fill + border * (1 - fill.a)` where `fill = vec4(fill_lin,
  fill_color.a * aa_inner)` — i.e. `fill.rgb` was left as STRAIGHT
  (non-premultiplied) `fill_lin`, never actually scaled by its own alpha,
  while the composite formula itself is the premultiplied-alpha "over"
  pattern. Combined with the pipeline's `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`
  blend state (which expects straight-alpha shader output and does its
  own alpha scaling during blending), the un-premultiplied `fill_lin`
  got scaled by the OUTPUT alpha a second time, inflating RGB well past
  1.0 after blending — visible as a solid, oversaturated tint instead of
  a thin ring. Confirmed via CPU-side (`Ca_DrawCmd`) and GPU-SSBO
  (`Ca_RectPushConst`) diagnostics that the INPUT data (border_width=1.5,
  corner_radius=8, correct color) was correct at every stage up to the
  shader — this was purely a shader-math bug, not a data-plumbing one.
  **Fix:** replaced the premultiplied-style formula with a proper
  coverage-weighted straight-alpha mix — `aa_inner` (interior coverage)
  and `bm` (ring coverage) are complementary and sum to `aa_outer`, so
  `out_a = fill.a*aa_inner + border.a*bm` and `out_rgb =
  (fill_lin*fill.a*aa_inner + border_lin*border.a*bm) / out_a` (guarded
  for `out_a≈0`). This is a real, general engine bug — affects ANY
  future border-width use on a translucent fill, not just this feature;
  never previously exercised since this focus indicator was the first
  and only caller of `border-width`/`border-color` in the whole
  Sol + Causality tree (checked: zero other selectors use it).
- **Bug 2 — border painted pre-children, so children painted over its
  corners.** After fixing Bug 1, user's next screenshot showed a clean
  thin border on straight edges but SHARP SQUARE corners on a panel with
  `border-radius: 8px`. Verified via a standalone C port of the exact
  GLSL SDF math (`roundedBoxSDF`/`smoothstep` reimplemented and sampled
  along the corner arc) that `bm` (border ring coverage) does NOT
  collapse to zero at the corner — the shader math itself is correct
  there. Root cause was paint ORDER, not math:
  `paint_tree_cached` (causality/src/ui/paint.c) calls
  `paint_node_content` (background + border, pre-children) BEFORE
  recursing into children, then a separate post-children phase for
  scrollbars only. `.buffer-pane`'s children (`.buffer-tabs-row`,
  `.buffer-body`) are laid out flush against the parent's content
  box edge-to-edge by design (the whole point of the "floating glass"
  composition) and carry their OWN corner rounding — so they paint
  directly over the parent's border ring wherever their edges coincide,
  which is exactly at the corners (their own rounding consumes the same
  pixels the parent's border arc occupies), while the short straight-edge
  slivers between child and parent edge survive uncovered. This is not
  standard CSS box-model behavior (a border should sit outside content,
  immune to children) — it's a genuine paint-order gap in the engine.
  **Fix:** extracted all border painting (uniform + per-side) out of
  `paint_node_content` into a new `paint_border()` function, called from
  `paint_tree_cached`'s POST-children phase alongside the existing
  `paint_scrollbars()` (which already solved the identical problem for
  scrollbar thumbs — same pattern, same cache-batching). Uses `own_clip`
  (the node's own bounds re-intersected with its own `corner_radius`,
  already computed for scrollbars) rather than the plain inherited
  `clip`, so the border's clip push-constant correctly advertises this
  node's own radius rather than inheriting the ancestor chain's
  (typically 0, per a `[clip-dbg]` diagnostic that showed `clip.radius=0`
  even though `node.corner_radius=8` — ruled out as the corner-squaring
  cause itself, since a same-size rectangular scissor can never clip a
  strictly-smaller rounded shape's corners, but still more correct to
  fix). The border draw command now has a fully transparent fill
  (`draw_mode=NORMAL`, r/g/b/a left at 0 from `memset`) so only the ring
  paints — the node's real background already painted in the
  pre-children pass underneath it.
- Also changed the indicator color from blue to bright orange per user
  request (`SOL_UI_FOCUS_BORDER_COLOR_CSS` → `rgba(255, 140, 0, 0.95)`
  in `sol_ui_constants.h`); width stays 1.5px.
- Build + full CTest suite (14/14) pass; `git diff --check` clean on Sol
  and the Causality submodule; stable launch with zero log output under
  both plain and `VK_LAYER_KHRONOS_validation` runs. All diagnostic
  `fprintf` instrumentation added during investigation (in `paint.c` and
  `swapchain.c`) was removed before the final build — `swapchain.c` kept
  its added `#include <stdio.h>` since the file already used
  `fprintf`/`printf` extensively elsewhere and the include was genuinely
  missing.
- Orange color confirmed working immediately. Corner rounding was NOT
  fixed — see Round 10.

## Round 10 — rounded border corner: three more root-cause attempts, all
disproven; shipped a square-corner border instead

User kept reporting the border corner as "weird"/"broken", with useful
zoomed screenshots (native-resolution crops, not just full-window shots)
letting this be checked far more precisely than in Round 9.

- **Hypothesis A — stroke too thin to render a visible curve.** Wrote a
  standalone C reimplementation of the exact GLSL `roundedBoxSDF`/
  `smoothstep` formula and rendered the corner as ASCII art at 1.5px vs
  thicker widths — 1.5px showed a sharp 1-2px transition, 2.0-2.5px
  showed a much more gradual multi-row taper. Bumped border width
  8→2px. **User's next screenshot: still broken, AND now "too thick."**
  This hypothesis was wrong, or at best incomplete — width alone wasn't
  the cause, since the defect persisted regardless of width.
- **Hypothesis B — scissor truncation shaving the outer edge.** Found
  that `swapchain.c`'s clip→scissor conversion truncates (not rounds)
  both the origin AND the far edge to physical pixels
  (`(int32_t)(clip_x * scale_x)`), which can end up up to ~1 physical
  px short of the true right/bottom edge at fractional DPI scale — a
  plausible corner-specific miss for a thin ring hugging that edge.
  Changed it to floor the origin / ceil the far edge (`floorf`/`ceilf`)
  so the scissor never under-clips. This is a renderer-wide change
  (every clipped draw command, not just borders) — flagged the wider
  blast radius to the user explicitly before considering it validated.
  Reverted border back to 1.5px per the "too thick" feedback in the
  same round. **User's next screenshot (a much more precise, near-native-
  resolution crop): still shows the defect**, now visible with total
  clarity as a small diagonal CHAMFER/bevel where the vertical and
  horizontal border segments meet — not a smooth arc, not a total gap,
  a literal cut corner — while the panel's own filled background shows
  a proper, generously-rounded curve right behind it. This ruled out
  Hypothesis B too (or at least proved it wasn't sufficient).
- **Hypothesis C — re-verified the SDF formula a third, most rigorous
  time**, specifically to check whether the "smooth" ASCII renders from
  Round 9/10A were actually smooth or whether ASCII-art coarseness was
  hiding the same chamfer: traced the OUTER edge boundary explicitly
  (scanned every pixel, printed coordinates where `|d_outer| < 0.5`) at
  the same 8px radius / 1.5px width as the failing screenshot. The
  traced boundary is a genuinely smooth, gradually-stepping arc
  (`(0,54)→(1,55)→(1,56)→(2,57)→(2,58)→(3,58)→(4,59)...`), not a jump
  from vertical straight to horizontal straight. **The formula is
  provably correct in isolation, a third independent way** — yet the
  live GPU output still shows a chamfer. Checked and ruled out: stale
  standalone `/Users/duke/Code/causality` repo shadowing the submodule
  (confirmed `vendors/causality` is a real directory, not a symlink, and
  CMake's `add_subdirectory(vendors/causality)` is a relative path that
  can only resolve inside Sol's own tree); the border accidentally
  triggering `CA_DRAW_MODE_SHADOW` instead of `NORMAL` (explicitly zero
  both via memset and an explicit assignment); the per-side border-rect
  fallback also firing (confirmed no per-side `border-{side}-{width,
  color}` CSS exists anywhere in Sol, so `border_top_w` etc. all stay
  zero and that loop's `continue` guard always fires); corner_radius or
  border_width silently differing between the pre-children fill's paint
  call and the post-children border's paint call (both read the same
  `node->desc` fields, same node, same frame — provably identical); the
  background-blur mask (a separate, C-driven, normalized-0..1-radius
  system) using a different effective radius than the CSS value (traced
  its call site — same `SOL_UI_PANEL_RADIUS_PX`-derived value, same DPI
  scale applied).
- **Concluded: this is a GPU-execution-level discrepancy that doesn't
  reproduce in a from-scratch CPU reimplementation of the same math** —
  the same category of dead-end as [[backdrop_blur_removed]]'s menu-popup
  blur investigation (proven-correct Vulkan state, zero pixels on
  screen; needed a Metal GPU frame capture to go further, never deemed
  worth it). Surfaced this explicitly to the user rather than continuing
  to guess after three independent, all-clean verification passes.
- **User chose: keep the border, drop the rounded corner** (of the three
  options offered: keep investigating with GPU tooling, revert the
  border entirely, or ship it square). Implemented in `paint_border()`
  (causality/src/ui/paint.c): the border draw command now always sets
  `corner_radius`/`corner_tl/tr/br/bl = 0` regardless of the panel's own
  rounding — a deliberate, explained divergence from `node->desc`, not a
  reversion of Round 9's post-children ordering fix (which stays; it's
  still what makes the border visible over children at all, corner
  shape aside). Since a square corner would otherwise poke past the
  panel's own rounded silhouette (the panel's arc is *inside* where a
  naive square corner would reach), the border rect is inset by
  `radius * (1 - 1/√2) ≈ 0.293 * radius` on all sides — the standard
  "largest inset square that stays inside a radius-r rounded corner"
  formula — clamped so the inset never exceeds half the node's smaller
  dimension (degenerate/tiny nodes). At the panel's 8px radius this is
  a ~2.3px inset, comfortably clearing the curve.
- Left the scissor floor/ceil fix (Hypothesis B) in place even though it
  didn't resolve the corner bug on its own — it's still a real,
  independently-justified correctness fix (never under-clip a legitimate
  edge pixel) with no observed downside, just not sufficient alone.
- Build + full CTest suite (14/14) pass; `git diff --check` clean on Sol
  and the Causality submodule; stable launch with zero log output under
  both plain and `VK_LAYER_KHRONOS_validation` runs.
- **If revisiting the rounded-border corner bug later:** the next real
  lead is GPU-side inspection (Xcode Metal frame capture, or a
  debug-only shader variant that writes `d_outer`/`d_inner`/`bm`
  directly to `out_color` for one frame) — everything reachable from
  reading C/GLSL source and reimplementing the math on the CPU has now
  been checked clean three times over.

## Round 11 — focused-panel border restoration

The square inset border introduced in Round 10 was a deliberate workaround for
the old rounded-border compositor, but it necessarily left visible gaps where
the square stroke stopped short of the rounded panel silhouette. Replaced that
workaround with a dedicated `CA_DRAW_MODE_BORDER_STROKE` path: it draws one
centered signed-distance stroke along the node's real per-corner radii and
uses derivative-based antialiasing, so a thin border remains continuous around
the arc without blending a transparent interior. `paint_border` now restores
the node's full bounds and per-corner radii for the post-children stroke. The
Sol focus indicator is reduced from 1.5px to 1px. Build and all 14 CTests
pass, both worktrees pass `git diff --check`, and a native startup produced no
runtime shader diagnostics. Visual confirmation remains the user's next live
run.

## Round 12 — border clip ownership

The user screenshot confirmed that Round 11's curved stroke itself was active,
but its arcs were entirely cut away. `paint_tree_cached` passed `own_clip` to
both the scrollbar and the post-children border. That is correct for a
scrollbar, which needs the panel's rounded overflow mask, but wrong for the
border: the border-stroke SDF already defines the panel silhouette, and the
second hard rounded clip removes its antialiased corner coverage. Borders now
use only the inherited ancestor clip, while scrollbars retain `own_clip`.
Build and all 14 CTests pass; both worktrees pass `git diff --check`.

## Round 13 — focused CSS radius parity

The next user screenshot proved the Round 12 change had removed the double
clip, but the focused panel's resolved border radius was zero, producing a
sharp rectangle. The focused modifier must explicitly carry the panel radius:
it now declares the shared default in the fallback stylesheet, and the live
appearance overlay applies the configured radius to every focused panel class.
The border therefore follows the same radius as the active theme and Settings
appearance value, rather than relying on cross-class radius resolution. Build
and all 14 CTests pass; both worktrees pass `git diff --check`.

## Round 14 — panel radius ownership

The focused CSS rule correctly declared a radius, but the screenshot remained
square because the Causality panel descriptor still carried zero: its CSS value
was not the same value Sol used for the rounded blur mask. Panel construction
is now the authoritative handoff. Tree/plugin, buffer, terminal, and welcome
panel descriptors receive `SolSettings.corner_radius` directly (or the shared
default before settings load), which Causality scales and preserves for the
post-children border command. Blur, fill, clipping, and the focus stroke now
use the same configured radius. Build and all 14 CTests pass; both worktrees
pass `git diff --check`.

## Round 15 — explicit rounded-border geometry

The screenshot still rendered a square border after radius propagation, so the
problem was isolated to the single-command uniform border ring, not CSS or
panel geometry. Removed that ring path. Uniform borders now render as four
thin straight segments plus a tangent sequence of normal rounded-rectangle
stamps around each actual corner radius, all inside the panel's own rounded
clip. This avoids the failing transparent-fill/border shader path entirely
while preserving post-children paint order and CSS border colours/widths.
Build and all 14 CTests pass; both worktrees pass `git diff --check`.

## Validation

- `cmake --build build --parallel 6`: passed.
- `ctest --test-dir build --output-on-failure`: 14/14 passed.
- Added a visitor geometry assertion proving an 8 px rendered divider produces
  an 8 px leaf gap.
- `git diff --check`: passed for Sol.
- `git -C vendors/causality diff --check`: passed. Causality changes cover the
  rounded image/backdrop shader and explicit per-corner CSS semantics.
- Native validation-layer launches passed for the welcome state and with
  `sol/src/ui/workspace.c` open. The rounded blur pipeline compiled, localized
  blur resources initialized, all 13 plugins loaded, and no Vulkan validation
  errors were emitted through clean runtime observation.
- Host `screencapture` still reports `could not create image from display`
  (no screen-recording permission in this environment) — could not obtain a
  fresh visual capture this session either. Diagnosis instead used a live
  instrumented run (`~/.sol/settings.json` has `corner_radius: 20`,
  `panel_opacity: 0.33`, `panel_blur: 0`, theme `com.sol.theme.monokai` +
  effect `com.sol.bfx.waves` — a different theme/session than the screenshot
  supplied with the bug report) plus exhaustive static verification of the
  blur-region and rect-pipeline shader math, cross-checked against real
  logged region/radius values. A pixel-level before/after comparison of the
  `.scm-tab`/`.pm-badge` fix is still outstanding — needs either
  screen-recording permission granted to the terminal, or a Vulkan
  frame-capture/readback path (none exists in the codebase yet) to verify
  visually. Build and full CTest suite pass; `git diff --check` clean.
