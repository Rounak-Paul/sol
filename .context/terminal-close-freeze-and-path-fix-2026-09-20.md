# Terminal Close Freeze + Login-Shell PATH Fix — 2026-09-20

## Reported symptom
User ran Claude Code in Sol's integrated terminal, then clicked the tab
"x" close button. The whole app froze. Separately, the integrated
terminal could not find `cmake` despite it being installed via Homebrew.

## Root causes found and fixed

### 1. Terminal close freeze — `sol_terminal_stop_pty` (sol_terminal.c)
`kill(term->child_pid, SIGTERM)` signaled only the shell process, not its
process group. Interactive shells put a foreground job in its own pgid
via `setpgid`, so a foreground command the shell launched (Claude Code,
or anything else) was never signaled and could keep the PTY session
alive after the shell itself died/was killed. Fixed by using
`killpg(term->child_pid, ...)` instead — `forkpty`'s child is a session
leader via `login_tty`, so `child_pid` doubles as the pgid.
Also hardened: the master-fd close before `pthread_join` was previously
gated on `child_pid == 0`; made unconditional (closing our own fd is
what actually unblocks the reader thread's blocking `read()`, verified
empirically on macOS — reaping alone is not sufficient/timely enough
against a killpg()'d grandchild).

**Investigated and ruled out** (don't re-chase these): lock-based
AB-BA deadlock via `ca_instance_wake()` — traced fully, it's a bare
unlocked `glfwPostEmptyEvent()`/`[NSApp postEvent:]`, no Sol/Causality
mutex involved anywhere in the click→close→join call chain. Reentrant
`drain()` during close — traced, click dispatch and `drain` are
sequential phases of one `ca_instance_tick()`, never nested.

### 2. cmake/Homebrew not found in terminal — `sol_terminal_start_pty`
The PTY child exec'd the shell as `execvp(shell, {shell, NULL})` — plain
argv[0], no login-shell marker. zsh only reads `/etc/zprofile` and
`~/.zprofile` (where `brew shellenv` lives) when invoked as a login
shell (argv[0] starts with `-`). Fixed by launching with
`argv[0] = "-" + basename(shell)` and `execv` (shell path from $SHELL is
always absolute, so the `p`-search variant was never needed). Matches
what Terminal.app/iTerm2/xterm all do. Verified: `zsh -c 'which cmake'`
fails, `zsh -l -c 'which cmake'` finds `/opt/homebrew/bin/cmake`.

## Additional hardening from a full audit (user asked for cleanup pass)

- **NULL-deref crash risk (confirmed)**: `vt_set_dec_mode` case 47 and
  `sol_terminal_resize`'s alt_screen growth both called `term_line_alloc`
  without checking its return, then set `alt_screen_rows` to the full
  target count regardless — an OOM mid-loop left rows with NULL `cells`
  that the alt-screen pointer-swap and `term_put_char` then dereference
  unconditionally. Fixed by clipping `alt_screen_rows` to the last
  successfully-allocated row in both sites (mirrors the pattern the main
  `screen[]` growth path already used correctly), and bounding the
  swap-in/swap-out loops to `min(alt_screen_rows, term->rows)`.
- **Infinite-spin hang guard (defensive, unconfirmed reachability)**:
  `on_term_tab_click`/`on_term_tab_close`'s tab-navigation loop had no
  bound — if ever reached with a stale/out-of-range `tab_index` (manager
  emptied since the context was captured), `active_index` could never
  reach it and the loop spins the UI thread forever. Added a bounds
  check before entering the loop, factored into a shared
  `term_tab_navigate_to` helper.
- **Duplication cleanup**: merged `TermTabClickCtx`/`TermTabCloseCtx`
  (identical fields) into one type; merged the two identical
  navigate-to-tab while-loops into `term_tab_navigate_to`; moved
  `sol_terminal_reader_deposit` out of the `#if !defined(_WIN32)` block
  (changed its count param from `ssize_t` to `size_t` for MSVC
  compatibility) so the Windows ConPTY reader thread reuses it instead of
  hand-duplicating the ring-buffer-deposit logic; factored
  `sol_terminal_send_text`'s SSH-vs-PTY write loops into a shared
  `term_transport_write_once` primitive called from one retry loop.
- **SSH connect freeze risk (confirmed, partially fixed)**: the whole
  connect→handshake→auth chain runs synchronously on the single UI
  thread with no timeout anywhere — an unreachable host or a stalled
  handshake could freeze the entire editor (not just the SSH tab) for
  the OS's TCP timeout (60-130s) or indefinitely. Fixed the two bounded
  parts: TCP connect is now non-blocking-with-`select()` timeout
  (`SOL_SSH_CONNECT_TIMEOUT_MS` = 10s, verified against a black-hole
  address), and `libssh2_session_set_timeout` bounds handshake/auth
  (`SOL_SSH_BLOCKING_TIMEOUT_MS` = 15s). **Not fully fixed**: this is
  still synchronous on the UI thread — a real fix would move the whole
  connect sequence to a background thread with a "connecting…" tab
  state; that's a bigger architectural change, deferred, flagged here to
  revisit if SSH connect freezes are ever reported in practice now that
  the worst-case is at least bounded to ~25s instead of unbounded.
- **Windows ConPTY join risk (flagged, not fixed — no Windows box to
  verify)**: `sol_terminal_stop_pty`'s Windows variant closes pipe
  handles then `pthread_join`s with no `CancelSynchronousIo`/timeout;
  unlike POSIX `close()` racing a blocked `read()`, this isn't a
  documented-reliable unblock pattern on Windows. Worth a real fix if
  Windows is actively used/tested.
- **Ruled out** (audited, no issue found): tab-context stale-pointer/UAF
  (contexts store `tab_index`, never a raw `SolTerminal*`); array bounds
  on `SOL_TERM_MAX_TABS`/`SOL_TERM_SCROLLBACK_MAX`/screen rows; resize
  duplication (too small to matter, each transport branch is 2-3 lines).

## Files changed
- `sol/src/core/sol_terminal.c`
- `sol/src/ui/terminal_panel.c`

## Verification
- `cmake --build build -j` clean, zero new warnings (one pre-existing
  unrelated `unused-includes` clangd note on `sol_threading.h`, not
  touched by this change).
- `ctest` 19/20 (the one failure, `causality_shader_cache_tests`, fails
  identically on unmodified `main` — confirmed via `git stash`,
  unrelated to this change).
- Root-caused via standalone forkpty/killpg reproductions (not just code
  reading) — confirmed `kill(pid)` vs `killpg(pid)` behavior, confirmed
  `close()`-unblocks-blocking-`read()` on macOS empirically, confirmed
  login-shell PATH difference via `zsh -c` vs `zsh -l -c`, confirmed the
  non-blocking-connect-with-timeout pattern against an RFC 5737
  black-hole address (3s bound held exactly).
- Not verified: interactively re-clicking "x" in the running app itself
  (no screen-recording permission in this shell — see
  [[screencapture_unavailable]]). The fix is proven at the mechanism
  level (killpg reaches grandchildren; login shell sources PATH) rather
  than by reproducing the exact freeze end-to-end in the live UI.
