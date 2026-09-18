# Power-Loss Recovery / Startup Corruption

## Symptom

After an unclean shutdown (power cut), Sol fails to start. Two shapes, depending on
whether the Vulkan validation layer is installed:

- validation layers present: SIGSEGV inside `vkCreateShaderModule`, crash report written
- release-like (no layers): `vkCreateGraphicsPipelines failed: -3`, `exit(1)`, no window

## Root cause

`~/.sol/shader_cache/*.spv` holds compiled SPIR-V fed straight to the GPU driver at
startup, before the window exists (`ca_window_create` -> `ca_renderer_window_init` ->
`ca_rect_pipeline_create` -> `ca_shader_compile`).

Two independent defects:

1. **No integrity validation on read.** `ca_shader_cache_lookup` accepted any file that
   was non-empty and 4-byte aligned. A truncated cache entry keeps a valid SPIR-V magic
   number and stays word-aligned, so it passed and reached the driver. The recompile
   fallback in `shader.c` only triggers when `vkCreateShaderModule` *returns* an error —
   drivers fault inside their own parser instead, so the fallback was never reached.

2. **No durability on write.** No `fsync` anywhere in either repo. `rename()` is atomic
   against process crashes but not power loss: the rename can reach disk while the data
   behind it is still in the page cache.

Note: multi-project/session state is purely in-memory (no persistence anywhere), so it
was never implicated despite being in use when the power cut happened.

## Why structural validation alone is insufficient

Walking the SPIR-V instruction stream rejects most truncations, but a cut landing
*exactly* on an instruction boundary yields a module that parses cleanly while its body
is missing — undefined forward references then fault in the driver. Empirically 1 of 24
truncated entries hit this case. Integrity must be guaranteed by the cache, not inferred
from the payload.

## Fix

**Container format** (`shader_cache.c`) — cache files are now a 5-word header
(magic `SCAC`, version, payload byte length, FNV-1a 64-bit digest as two words) followed
by the SPIR-V payload. Lookup verifies magic, version, length-vs-actual-size, and digest,
then still structure-checks the payload. Any mismatch is a miss: the caller recompiles and
overwrites. Old-format files fail the magic check and are transparently rewritten, so the
cache self-heals with no migration step.

**Durability** — `fsync` (macOS `F_FULLFSYNC` where supported, `fsync` fallback) before
every atomic rename, plus a directory `fsync` after the shader-cache rename so the
directory entry itself is durable. Added `sol_platform_sync_file()` /
`sol_platform_process_id()` to the platform layer; Causality keeps local static helpers
since its platform layer is window/menu-only.

Applied to: shader cache, `sol_settings_save`, `sol_text_buffer` saves (user file data),
and `sol_ssh_config_save` — which additionally wrote **in place** (`fopen(path,"wb")`),
truncating the live file with no temp+rename at all; now atomic like the others.

## Verification

Reproduced by truncating every `.spv` to a 4-aligned prefix in a sandboxed `$HOME`:
crash before the fix (both layer configurations), clean start after. Confirmed the cache
self-heals (corrupt entries rewritten with `53434143` container magic, including the
boundary-aligned one), warm restart reads the new format, and no orphan temp files remain.
Also verified recovery with zero-length `settings.json` + `ssh_connections.json`.

Regression tests in `ca_shader_cache_tests.c`: `test_truncated_payload_is_a_miss` (cut on
an instruction boundary) and `test_altered_payload_is_a_miss` (same length, flipped byte).
Pre-existing fixtures used 2-4 word dummy blobs that are not valid SPIR-V physical layouts;
updated to real minimal modules now that lookup validates structure.

Full suite: 19/19 pass. Live `~/.sol` was never modified (all runs used sandboxed HOMEs).

## Second sweep — additional bugs found (same session)

Found by fuzzing the config loaders and crash handler, not by reading code.

### 1. Infinite hang on corrupt ssh_connections.json (startup hang)

`sol_ssh_config_load`'s array loop called `jp_parse_connection`, which calls
`jp_expect(j, '{')` — and that returns false **without advancing the cursor** on a
non-`{` character. A file like `[}]}]}` (reachable from a truncated/hand-edited save)
spun forever: Sol hung at startup with no error and no crash report.

Fixed by recording the cursor before each element and skipping one byte if nothing was
consumed, so the scan always makes progress. A file with junk followed by a valid entry
now still recovers the valid entry.

The `settings.json` parser was fuzzed with the same corpus and does **not** have this bug —
its object loops `break` on a failed key/colon parse. Arrays have an element loop that
objects don't, which is why only the SSH path was affected.

### 2. Process wedge on a fault inside the crash handler

`sigaction` used `SA_SIGINFO` with `sigemptyset(&sa.sa_mask)` and no alternate stack.
A fault raised *inside* the handler (damaged stack faulting in `backtrace()`) is a
synchronous signal that cannot be delivered because it is blocked during handling — on
macOS that wedges the thread permanently. Verified empirically: the test process printed
`handler depth=1` and then hung forever, never dying, never reporting.

Fixed with `SA_NODEFER | SA_ONSTACK` plus a `sigaltstack` and a `sig_atomic_t` reentrancy
guard. `SA_NODEFER` lets the nested fault be delivered, the guard catches the re-entry and
terminates, and `SA_ONSTACK` keeps that second entry off the exhausted/corrupt stack —
which also makes stack-overflow SIGSEGVs reportable for the first time. Verified: nested
fault now terminates with 139 instead of hanging, and normal crash reports still produce
full backtraces.

Note: the guard alone did **not** work (the handler was never re-entered — the kernel wedged
before dispatch). `SA_NODEFER` is the load-bearing part; the guard prevents the resulting
infinite re-entry loop.

### 3. Empty bindings.conf silently disabled every keyboard command

A zero-length `bindings.conf` (interrupted first-launch write) parsed fine, registered zero
bindings, and set `input_binding_active = false` — no save, no open, no explorer, and **no
diagnostic whatsoever**. Because the file existed, defaults were never re-emitted.

Now a zero-length file is treated as absent (defaults regenerated), and any config that
parses but binds nothing prints a warning naming the file. A comments-only config is
preserved rather than clobbered — it warns but keeps user content.

### 4. Shader cache temp name was not actually per-process

The temp path used the SPIR-V buffer's heap address while its own comment claimed
"per-process". Two instances compiling the same shader could collide and tear the write
the rename exists to prevent. Now uses pid + pointer. Verified with 4 concurrent instances
on a cold shared cache: no crashes, no orphan temp files, all entries valid.

## Third sweep — completion pass

Ran ASan + UBSan mutation fuzzing over every corruption-reachable parser. Results:

- **ssh_connections.json**: 140 mutated inputs — 0 memory errors, 0 hangs (post-fix).
- **settings.json**: 133 mutated inputs — 0 memory errors, 0 hangs. Confirms this parser
  never had the array-loop hang; only arrays have an element loop.
- **shader cache container**: 133 mutated binary inputs — 0 memory errors, 0 hangs, only
  the 2 genuinely valid containers accepted; everything malformed correctly rejected.
- **bindings.conf**: adversarial tokenizer inputs (5000-char tokens, 500-step sequences,
  2000 tokens/line, binary garbage, missing action, no trailing newline) — all handled.
- **crash report path builder**: exhaustively tested every out_size 0..140 against every
  directory length 0..120 on exact-size heap buffers under ASan — no overflow. The manual
  bounds checks are correct; an undersized buffer makes it fail cleanly, never overrun.

### Last non-durable write fixed

`sol_write_default_bindings` still wrote `bindings.conf` directly. A power cut during the
first-launch write is exactly what produces the empty-config state fixed above. Now uses
temp + fsync + atomic rename like every other config write, so that state is not produced
in the first place rather than only healed afterwards.

### Durability audit — final state

Every config write in the repo is now temp-file + fsync + atomic rename:
`sol_settings_save`, `sol_ssh_config_save`, `sol_text_buffer` saves,
`sol_write_default_bindings`, and `ca_shader_cache_store` (which also fsyncs the directory).

The two remaining direct `fopen(..,"wb")` sites are correct as-is:
`sol_platform_copy_path_recursive` (a file-copy utility, not config persistence) and the
shader cache's `.ca_write_probe` (created and deleted immediately to test writability).

## If it recurs on another machine

Deleting `~/.sol/shader_cache/` is a safe manual workaround — it is a pure cache and is
rebuilt on next launch.
