# Windows Plugin Loading

## Problem
- Plugins loaded on macOS/Linux but Windows startup could miss plugin DLLs.
- Two fragile boundaries were involved:
  - Runtime discovery used `argv[0]` and only searched for `/`, so Windows paths like `D:\...\sol.exe` did not reliably resolve to the executable directory.
  - Plugin targets only specified `LIBRARY_OUTPUT_DIRECTORY`; Windows DLL artifacts are runtime outputs and need `RUNTIME_OUTPUT_DIRECTORY` as well.

## Fix
- Added `sol_platform_get_executable_path()` in `sol_platform`:
  - Windows: `GetModuleFileNameA(NULL, ...)`
  - macOS: `_NSGetExecutablePath(...)`
  - Linux/Unix: `readlink("/proc/self/exe", ...)`
- Startup now resolves `plugins` beside the actual running executable, falling back to `argv[0]` only if the platform query fails.
- Plugin CMake targets now call `sol_configure_plugin_output(target)`, which sends both module-library and runtime artifacts to `$<TARGET_FILE_DIR:sol>/plugins`.

## Validation Notes
- Build should place Windows plugin DLLs under the same directory as `sol.exe`, inside `plugins`.
- Loading no longer depends on the current working directory or on `argv[0]` containing a slash.
