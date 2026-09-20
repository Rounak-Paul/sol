# Terminal X-Button Close Freeze — 2026-09-20

## Symptom
User report: `L t x` (leader-key kill terminal) reliably closes a terminal
tab with a running foreground job. Clicking the `×` button on the terminal
tab in the UI, for the same running job, freezes the whole app instead.

## Root cause
`sol_terminal_stop_pty()` in `sol/src/core/sol_terminal.c`:

1. Sends `killpg(SIGHUP)` + `killpg(SIGTERM)`, polls `waitpid(WNOHANG)` up
   to 20×10ms (200ms total).
2. If still not reaped, escalates to `killpg(SIGKILL)`, then previously did
   a **plain blocking `waitpid(child_pid, &status, 0)` with no timeout** —
   the only unbounded wait in the whole teardown path.

SIGKILL cannot be blocked/trapped, but the *process* can still take an
unbounded amount of real time to actually become reapable if it (or a
same-process-group child that re-parented into its own group while still
holding the PTY slave fd open — exactly what an interactive CLI tool like
`claude` does) is deep in an uninterruptible kernel wait. That blocking
`waitpid` was the freeze.

`Ltx` (`terminal.kill` in main.c:2000) "worked" only by accident: it calls
a separate `sol_terminal_kill()` which does a raw `kill(child_pid,
SIGKILL)` on the shell *before* `sol_terminal_manager_close_active()` even
runs. That pre-kill almost always makes `stop_pty`'s very first `WNOHANG`
poll iteration already see the shell reaped, so the code path never
reaches the unbounded blocking `waitpid` at all. The X button
(`on_term_tab_close` in `terminal_panel.c`) calls `close_active` directly
with no pre-kill step, so it was the one path that reliably hit it.

## Verification method
Did NOT rely on static tracing alone (see memory
`feedback_verify_dont_rationalize`). Built a standalone harness that links
the real `sol_terminal.c.o` (+ `sol_platform.c.o`, libssh2, mbedtls,
libutil) against a fake `Ca_Instance` pointer and a stub `ca_instance_wake()`
— no GLFW/Vulkan/window needed since the manager never dereferences the
instance outside real rendering. Harness spawns a real `forkpty` login
shell, sends `claude\n` as the foreground job (matching the user's exact
repro), waits for it to start, then calls the manager's close path exactly
as the UI button does. Confirmed:
- Before fix: hangs 20s+ (killed manually), `sample` showed the main
  thread parked in `__wait4` inside `sol_terminal_destroy`; `ps` showed
  the shell as a genuine zombie of the harness process that a manual
  external `kill -KILL` also couldn't clear promptly, and `claude` as an
  orphan with its own process group (not reachable by `killpg` on the
  shell's group).
- After fix: consistently returns in ~0.49s across 4 runs, no zombies or
  orphans left behind afterward (`ps -eo stat` checked for `Z`).

## Fix
Bounded the post-SIGKILL wait with the same 20×10ms `WNOHANG` poll used
pre-SIGKILL. If it still hasn't reaped after that, stop waiting
synchronously on the UI thread — SIGKILL was already delivered, so the
kernel will finish the job — and hand the final reap off to a new detached
helper, `sol_terminal_background_reap()`, spawned via `pthread_create` +
`pthread_detach`, so the zombie doesn't leak for the rest of the app's
lifetime. Closing the master fd (unconditional, right after, unchanged)
is what unblocks the reader thread; it never depended on this reap
completing.

Also added `#include <stdint.h>` explicitly (`intptr_t` used to box the
pid for the thread's `void *arg`).

## Not yet done
- Not interactively verified in the real running `./bin/Sol` app (no
  screen-recording permission in this shell — see memory
  `screencapture_unavailable`). Ask the user to click the X button on a
  terminal tab running `claude` (or another interactive CLI) to confirm.
- `sol_terminal_kill()` (the `Ltx` path) itself still uses raw `kill()`
  instead of `killpg()` — inconsistent with the intentional killpg fix
  documented in `terminal-close-freeze-and-path-fix-2026-09-20.md`. It
  happens to work today only because it's always followed immediately by
  `close_active`'s own killpg-based teardown. Flagging as a latent
  inconsistency, not touched in this pass since it wasn't the reported bug
  and changing it isn't needed for the fix to be correct.
