# Causality Title-Bar Default Styles

## Requirement

Causality-owned title bars must remain complete and readable when an application
provides no CSS or provides CSS unrelated to system chrome. Applications must be
able to override any default title-bar declaration without restating the whole
title bar.

## Architecture

- Each `Ca_Instance` owns a parsed system stylesheet containing only Causality
  chrome defaults.
- Style resolution applies the system origin first and the borrowed application
  stylesheet second. Author declarations therefore override system defaults.
- Widget initial styling, pseudo-state reapplication, runtime style changes,
  stylesheet refresh, synthetic overlays, and system title-bar nodes all use the
  same layered resolver.
- Title-bar menus propagate resolved item foreground colors to their child label
  nodes, including hover re-resolution.

## Default Coverage

- Title-bar surface and bottom separator
- Menu bar sizing, item typography, spacing, colors, and hover state
- Drag region and centered window title
- Control group, minimize/maximize/close buttons, and hover states
- Menu dropdown, selected item, and hover overlay surfaces

## Verification

- `cmake --build build -j2` passes.
- `ctest --test-dir build --output-on-failure` passes all 11 tests.
- The default and author sheets are parsed independently, avoiding rule or
  string-pool capacity coupling between the two origins.
