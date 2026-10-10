# Idle GPU with buffer open (2026-10-10)
- Cause: `sol_ui_on_frame` (workspace.c) bumped `sig_buffer_rev` + `ca_instance_wake()` every tick while a buffer was focused (and `ca_instance_wake()` for focused terminal blink) -> loop never slept, full rebuild+render per iteration.
- Fix: text_view's `caret_blink_visible` reports ms to next phase flip -> `sol_ui_system_schedule_caret_phase` stores earliest `ui->caret_next_phase_ns` and arms `ca_instance_request_frame_after`. on_frame bumps `sig_buffer_rev` only once due. Terminal blink uses request_frame_after for remaining toggle time.
- Idle CPU after fix: ~1-4% (README.md open). Not visually verified that caret still blinks.
