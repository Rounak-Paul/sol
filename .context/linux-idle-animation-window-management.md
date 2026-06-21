# Linux Idle Animation and Window Management

## Scope

- Empty-workspace hover transitions stalled in the event-driven loop.
- Custom title-bar dragging failed under Linux window managers, especially Wayland.
- Manual maximize geometry did not reliably match the compositor work area.

## Root Causes

- CSS transitions can start during Causality's input pass, after the frame's active-transition scan. With no active Sol buffer, no caret wake masked this ordering gap, so the next `glfwWaitEvents()` could block indefinitely.
- The title bar manually moved the undecorated window with `glfwSetWindowPos()`. Wayland intentionally does not support client-controlled top-level positions.
- Maximize manually copied monitor work-area geometry instead of asking the window manager to apply its maximize policy.

## Implementation

- `maybe_transition()` posts an empty GLFW event whenever it starts or retargets a transition.
- Title-bar drag start calls the vendored `glfwStartInteractiveMove()` extension. The GLFW backends use compositor/window-manager interactive move APIs on Wayland, X11, Win32, and Cocoa.
- Maximize and restore use `glfwMaximizeWindow()` and `glfwRestoreWindow()`.
- A GLFW maximize callback synchronizes Causality's custom title-bar icon/state with compositor-driven changes.

## Verification

- `cmake --build build -j2` passes.
- `ctest --test-dir build --output-on-failure` passes all 11 tests.
- The vendored GLFW backends were inspected: interactive move maps to
  `xdg_toplevel_move` on Wayland and `_NET_WM_MOVERESIZE` on X11.
- Native Linux interaction still requires a live compositor/window manager for
  final drag, maximize, and empty-workspace visual confirmation.
