# Causality Title-Bar & Menu Bar

## Custom title bar (current)

Windows are undecorated and Causality owns a custom `ca-titlebar` strip above
the content root. Query its resolved logical height with
`ca_window_get_title_bar_height`; it consumes content-space height.

## Title-bar menu API

```c
ca_instance_set_app_menus(Ca_Instance *instance, const Ca_MenuDesc *menus, int count);
```

- Menus are embedded in Causality's custom title-bar strip.

Causality deep-copies all descriptor data — caller may free immediately after.

## Sol Integration

- `sol_ui_rebuild_title_bar_menus` calls `ca_instance_set_app_menus(ui->instance, ...)`
- `sol_ui_system_title_bar_height` forwards Causality's resolved title-bar height.
- Workspace blur regions start below the title bar; this prevents left-panel blur
  boundaries from continuing into the title bar.

## CSS

- `.ca-titlebar-*` rules in `style.h` are still present; they style Causality's
  own chrome nodes on platforms where applicable (no-op on macOS native bar).
- `.ca-menubar-popup` styles the auto-emitted menu dropdown.
