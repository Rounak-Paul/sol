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

## If it recurs on another machine

Deleting `~/.sol/shader_cache/` is a safe manual workaround — it is a pure cache and is
rebuilt on next launch.
