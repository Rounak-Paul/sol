# Fast Launch — Deferred Startup Init

## Problem (2026-09-02)
`main()` blocked the first `ca_instance_tick()` (window creation/paint) on:
- A synthetic 100,000-iteration `sol_job_system_parallel_for` "warmup" job —
  dead code, its checksum was only read by a debug printf, never used
  functionally.
- Synchronous plugin directory scan + dlopen of every `.dylib` in `plugins/`.
- Saved theme / background-effect restore (depends on plugins being loaded).
- `SOL_EVENT_APP_STARTUP` / `SOL_EVENT_APP_READY` publish.

None of that is needed to paint the first frame — only Causality instance +
UI system + window creation is.

## Fix
- Removed `sol_warmup_range` / `SolWarmupContext` entirely (`sol/src/main.c`).
  Dropped `warmup_checksum` from `SolAppStartupPayload`
  (`sol/include/sol_event.h`).
- Extracted plugin loading, theme/bg-effect restore, and the
  startup/app-ready events into `sol_run_deferred_init(app, argc, argv)`.
- Frame loop now calls it once, gated by `app->deferred_init_done`, right
  after the **first successful `ca_instance_tick()`** — so the window and
  GPU pipeline (swapchain, font atlas, blur images) are already up before
  any plugin dylib loading or disk I/O happens.

## Verified
- Clean build (`cmake --build build --target sol`), no warnings.
- All 14 ctest suites pass.
- Real run confirms ordering: `[vk] swapchain created` / pipeline logs print
  before `[sol] startup: workers=N plugins=N input=ready`.

## Ordering note
Plugins can register custom themes/bg-effects. Deferring their load means
the window may show the built-in default theme for one frame before the
saved theme/effect applies — intentional trade-off, imperceptible in
practice, and `sol_ui_system_apply_appearance` already re-applies once
themes are restored. No plugin currently subscribes to `SOL_EVENT_APP_READY`,
so no external ordering contract was broken by deferring it.
