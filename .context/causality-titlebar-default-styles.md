# Causality Title-Bar & Menu Bar

## Decorated Windows (current)

Windows are now `GLFW_DECORATED = GLFW_TRUE`. The native OS title bar sits
outside the content area — it does not consume any content height.

- `ca_window_get_title_bar_height` **removed** — always treat as 0
- `ca_window_set_title_bar_menus` **removed** — replaced by `ca_instance_set_app_menus`

## App Menu API

```c
ca_instance_set_app_menus(Ca_Instance *instance, const Ca_MenuDesc *menus, int count);
```

- **macOS**: rebuilds `[NSApp mainMenu]` immediately.
- **Other platforms**: stored in `instance->app_menus`; `ca_ui_begin` auto-emits
  a `ca_menu_bar` widget with id `__ca_app_menubar__` at the top of the content
  root each frame. Sol does not emit this manually.

Causality deep-copies all descriptor data — caller may free immediately after.

## Sol Integration

- `sol_ui_rebuild_title_bar_menus` calls `ca_instance_set_app_menus(ui->instance, ...)`
- Title bar height is 0 everywhere; `sol_ui_system_title_bar_height` returns 0
- Blur regions no longer include a title-bar strip (removed from `sol_ui_update_blur_regions`)
- Background effect aux-window blur path removed the title-bar strip blur region

## CSS

- `.ca-titlebar-*` rules in `style.h` are still present; they style Causality's
  own chrome nodes on platforms where applicable (no-op on macOS native bar).
- `.ca-menubar-popup` styles the auto-emitted menu dropdown.
