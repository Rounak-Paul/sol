# Shader Compile Fix — 2026-06-20

## Current Issue
- Runtime GLSL compilation in vendored Causality fails while creating the UI system.
- The fragment shader in `vendors/causality/causality/src/renderer/pipeline.c` uses `half` as a local variable name.
- Shaderc rejects `half` as a reserved word, which aborts UI system initialization.

## Relevant Files
- `vendors/causality/causality/src/renderer/pipeline.c`
- `vendors/causality/causality/src/renderer/shader.c`

## Fix Strategy
- Rename the conflicting GLSL identifier to a non-reserved name.
- Rebuild the project to verify shader compilation and surface any additional issues.

## Verified Result
- `cmake --build build -j2` completed successfully after renaming the shader local.
- No additional build-time errors surfaced in the current tree.
- The app now gets past UI-system shader compilation.
