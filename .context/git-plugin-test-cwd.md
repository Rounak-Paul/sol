# Git Plugin Test CWD Fix — 2026-06-20

## Current Issue
- `sol_git_plugin_tests` passed when run from the repo root, but failed under `ctest`.
- The untracked diff test used a hard-coded relative path: `Plugins/sol-plugin-git/tests/test_git_model.c`.
- CTest launches the binary from the build directory, so that path was not valid there.

## Relevant File
- `plugins/sol-plugin-git/tests/test_git_model.c`

## Fix Strategy
- Resolve the repository root at runtime with `git rev-parse --show-toplevel`.
- Build an absolute target path for the no-index diff check.
- Re-run `cmake --build build -j2` and `ctest --test-dir build --output-on-failure`.

## Verified Result
- The Git plugin test suite now passes under both direct execution and CTest.
