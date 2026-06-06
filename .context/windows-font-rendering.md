# Windows Font Rendering Context

Task: improve Causality/Sol font rendering on Windows while preserving deterministic embedded-font output across platforms.

Relevant paths:
- `vendors/causality/causality/src/renderer/font.c`: FreeType-backed dynamic glyph atlas, LCD/grayscale raster policy, glyph load/render flags.
- `vendors/causality/causality/src/renderer/font.h`: glyph quad generation and baked atlas metrics.
- `vendors/causality/causality/src/renderer/pipeline.c`: text shader and blending for atlas coverage.
- `vendors/causality/causality/src/ui/paint.c`: text draw command generation, clipping, and pixel snapping.
- `sol/src/main.c`: creates `Ca_Instance` without a font override, so renderer defaults matter.

Findings:
- The app uses embedded `RobotoMonoNerdFontMono-Regular.ttf` and `RobotoMonoNerdFontMono-Bold.ttf`.
- Causality should not default to platform fonts; the desired product direction is deterministic embedded-font rendering across Windows, macOS, Linux, and screen densities.
- Unhinted high supersampling fixed cut edges but made 1x Windows text too blurry.
- Windows-like output needs native TrueType hinting plus LCD subpixel coverage for small low-DPI text, with grayscale fallback for HiDPI, icons, and larger text.

Implemented:
- Removed renderer-side platform font auto-detection from the default path.
- Replaced the embedded Departure Mono font with Roboto Mono Nerd Font Mono Regular/Bold from `C:\Users\Duke\Downloads\RobotoMono`.
- Updated embedded font metadata and vendor notice to point to the Roboto Mono Apache-2.0 source and the Nerd Fonts patcher.
- Added `display_scale` to the font object so output pixel snapping is separate from glyph rasterization.
- Small low-DPI regular text uses `FT_LOAD_TARGET_LCD` with native font hinting and FreeType's default LCD filter, then stores RGB subpixel coverage in the RGBA atlas.
- The text shader consumes RGB atlas coverage instead of sampling only the red channel, preserving subpixel coverage without platform font rendering.
- Font atlas sampling uses nearest filtering because atlas texels already contain final grayscale/LCD antialiasing; this avoids the blur introduced by an extra linear filtering pass.
- Larger low-DPI text uses controlled grayscale reconstruction; HiDPI uses native-scale grayscale.
- Low-DPI text glyph X positions are snapped to output pixels while HiDPI positioning remains fractional.
- Text clips include a small vertical pad so tight UI rows do not shave antialiased ascender/descender pixels.

Verification:
- Plain `cmake --build build --config Debug` fails in a non-developer shell because MSVC include paths are not initialized and `<stdbool.h>` is not found.
- `cmd /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build --config Debug'` passes.
- `cmd /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && ctest --test-dir build --output-on-failure'` runs successfully, but this build tree reports no registered tests.
- Directly running all `bin/*tests.exe` passes.
