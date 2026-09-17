# Git commit input crash investigation — 2026-09-17

## Confirmed unsafe path

- `plugins/sol-plugin-git/src/plugin.c:git_consume_task` clears the commit draft after a successful commit and clears drafts when repository discovery changes roots.
- The reactive panel rebuild passes that replacement string to `ca_input` in bundled Causality (`vendors/causality/causality/src/ui/widget.c`). Reconciliation reuses the input but previously reset cursor/selection only on initial creation.
- Clearing a previously edited input left its cursor beyond the new string length. `input_handle_keys` uses `len - cursor + 1` as a `size_t` in insertion/backspace `memmove`, allowing an enormous out-of-bounds copy on the next edit. Shorter replacements have the same issue.
- Fix: reset cursor to the replacement text's end and clear selection when descriptor text changes, matching `ca_set_text`. Unchanged text preserves cursor and selection across redraws.

## Evidence and validation

- Added regression to `causality/tests/ca_input_capture_tests.c`: reuse one input through clearing, shortening and UTF-8 replacement; type after each replacement; verify unchanged text preserves editing positions.
- Regression fails on the original code at the cursor invariant after clearing; passes with the fix.
- Full build and all 18 CTest targets pass. Root and Causality diff checks pass.
- Only pre-existing local Sol diagnostic report found was 2026-09-12: missing Vulkan library at startup, unrelated to typing. The user's exact interactive crash was not captured or reproduced; this is a confirmed unsafe code path consistent with the symptom, not proof of the specific incident.
- Fix resides in the Causality submodule working tree. No commits or pushes.
