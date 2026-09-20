# Workspace panel and scrollbar unification

## Scope

Unify the visual hierarchy of Explorer, contributed side panels (including Git),
buffer panes, and terminal panes. Make Causality-native overflow scrollbars use
the same semantic track/thumb treatment as Sol's custom buffer scrollbars in
both the Glass base theme and every generated palette theme.

## Current ownership

- `sol/src/ui/style.h` owns Sol's base Glass panel, tab, terminal, tree, Git,
  custom-buffer-scrollbar, and final floating-panel composition rules.
- `plugins/sol-plugin-themes/src/plugin.c::build_theme_css` emits the active
  semantic palette overrides. It must override native scrollbar properties as
  well as the custom buffer scrollbar classes.
- `vendors/causality/causality` resolves `scrollbar-width`,
  `scrollbar-track-color`, `scrollbar-thumb-color`,
  `scrollbar-thumb-active-color`, and `scrollbar-radius` onto overflowing
  nodes and paints the resulting overlays after their children.
- Explorer scrolls through `.tree-scroll-area`; Git panel content and Git
  custom output buffers use Causality overflow containers; buffers retain a
  custom logical scrollbar because their viewport is virtualized.

## Design contract

Every workspace panel has one outer surface, a compact raised header/tab strip,
and an inset content surface. Scrollbars are an overlay component: transparent
track, narrow rounded thumb, and a theme-primary active thumb. This makes native
overflow panels read like the buffer without giving them a separate visual
language or reserving layout width.

Use the shared classes for new workspace UI: `workspace-panel` for the shell,
`workspace-panel-chrome` for its header/tab strip, and
`workspace-panel-well` for content. The base Glass CSS, generated palette CSS,
appearance overlay, and Retro overlay all style these semantic roles.

## Implemented

- Glass makes the base native track transparent, retains the buffer's muted
  normal thumb and primary active thumb, and makes the Explorer, Git, buffer,
  and terminal headers share the raised chrome surface.
- Git now inherits the contributed side-panel shell instead of rendering a
  second opaque `.scm-root`; its 30px header is aligned with Explorer's header.
- Generated palettes explicitly apply the buffer scrollbar colors to every
  native scroll owner and distinguish the custom buffer's idle and active
  thumb states.
- Retro treats the Git root as a well and includes Git headers/tabs/section
  headers in the same raised-strip family as buffer and terminal tabs.
- Terminal content now shares the buffer body surface exactly: Glass puts the
  terminal panel on the same chrome surface as `.buffer-body`, palette themes
  put it on the same panel surface as `.buffer-pane`, and Retro puts its
  viewport/filler inside the same well as `.buffer-body`.
- Explorer, buffer, terminal, and Git renderers now attach the shared semantic
  role classes directly. Future panels can opt into the complete theme/style
  contract without adding a panel-specific colour family.
- Native scrollbars retain Causality's proven overlay renderer. They share the
  buffer's theme track/thumb colours through Sol CSS only; do not extend
  Causality's renderer or style descriptors for scrollbar beveling without a
  rendered regression test.

## Active follow-up: Causality scrollbar chrome

The color and geometry contract is insufficient for Retro: its custom buffer
scrollbar has a sunken track and raised thumb with directional bevel edges,
while Causality can currently only paint flat track/thumb fills. Extend
Causality's CSS-resolved scrollbar descriptor with optional track/thumb border
width and directional edge colors, then have the overlay renderer paint those
edges without changing scrollbar layout or hit-testing. Sol's Glass style
continues to use the flat buffer-equivalent treatment; Retro supplies the same
well/surface bevel tones as its buffer scrollbar.

Implemented on 2026-09-20. The new app-facing CSS properties are
`scrollbar-track-border-width`, `scrollbar-track-border-{top,right,bottom,left}-color`,
and their `scrollbar-thumb-*` counterparts. Causality paints them inside the
existing overlay rectangles, so the viewport reservation and drag hitboxes stay
unchanged. Glass's base track/thumb/active colors now exactly match the custom
buffer defaults; palette and appearance overlays retain their existing shared
color/geometry rules. Retro maps the native track to the buffer well's sunken
bevel and the thumb to its raised bevel. The Causality regression resolves and
paints all four directional track edges, and the Sol style test asserts the
Retro CSS contract.

Follow-up validation: `cmake --build build --target sol causality_splitter_tests
sol_style_tests --parallel 6`, the complete `ctest --test-dir build
--output-on-failure` suite (20/20), and both root/submodule `git diff --check`
passed. A local `bin/Sol.app` launch succeeded, but the desktop
accessibility/screenshot bridge timed out before a live window capture, so
rendered visual acceptance remains pending.

## Validation

`cmake --build build --target sol sol_plugin_themes sol_style_tests --parallel 6`,
full `ctest --test-dir build --output-on-failure` (20/20), and `git diff --check`
passed. The rebuilt app was launched; automated capture did not return a
window snapshot, so final rendered acceptance remains a live visual check.

On 2026-09-20, the currently running local `bin/Sol.app` process was verified
to have started at 20:49:25, before its executable was rebuilt at 21:31:56.
It therefore cannot contain the native scrollbar bevel change. Fully quit that
specific process after saving work, then launch the local bundle again before
using a screenshot as rendered acceptance.

Every current native overflow owner now carries `native-scrollbar` (Explorer,
file picker, plugin manager and switcher, search results and preview, Git
content, and Git diff output). Appearance, generated palette, and Retro rules
target that one semantic role, so new themes and panel surfaces no longer need
parallel selector lists. A Release build was installed to `/Applications/Sol.app`
with 13 plugins; executable, Themes plugin, and Git plugin SHA-256 hashes match
the local bundle. After a rendering regression was reported, all Causality
scrollbar extension changes were removed and the Release bundle reinstalled;
the Causality submodule is clean and full CTest remains 20/20.

The remaining missing title-bar/menu styling was caused by a Sol Themes plugin
format-argument ordering error: after adding the terminal panel colour, the
dark panel value was passed to the title-bar text slot. The first formatter is
now split into semantic writes, with title-bar labels explicitly mapped to the
theme secondary colour. Rebuilt executable and Themes plugin hashes match the
installed bundle.

## Local versus installed build

The rebuilt bundle is `bin/Sol.app`; it is separate from `/Applications/Sol.app`.
On 2026-09-20 the local executable modification time was 21:31 while the
installed executable was 12:33. Do not use the installed app as acceptance for
uninstalled workspace styling changes.
