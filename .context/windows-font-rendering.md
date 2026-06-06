# Windows Font Rendering Context

Task: improve Causality/Sol font rendering on Windows while preserving macOS quality.

Relevant paths:
- `vendors/causality/causality/src/renderer/font.c`: FreeType-backed dynamic glyph atlas, supersampling/reconstruction, glyph load/render flags.
- `vendors/causality/causality/src/renderer/font.h`: glyph quad generation and baked atlas metrics.
- `vendors/causality/causality/src/ui/paint.c`: text draw command generation and glyph positioning.
- `sol/src/main.c`: creates `Ca_Instance` without a font override, so renderer defaults matter.

Findings:
- The app normally uses embedded `RobotoMonoNerdFontMono-Regular.ttf` and `RobotoMonoNerdFontMono-Bold.ttf`.
- macOS looks better largely because content scale is commonly 2x, so grayscale glyphs have more physical pixels.
- Current glyph load flags force auto-hinting/light hinting for regular text. On Windows, native TrueType bytecode hinting is usually better for installed Windows fonts.
- Current paint code keeps X glyph positions fractional and the atlas sampler is linear. This is fine on HiDPI but makes grayscale glyphs soft/uneven on standard-density Windows displays.
- Causality should not default to platform fonts; the desired product direction is deterministic embedded-font rendering across Windows, macOS, Linux, and screen densities.

Change direction:
- Keep the embedded Causality font bundle as the default on every platform.
- Use one controlled FreeType path for regular text everywhere.
- Supersample embedded glyphs in Causality on low-DPI displays, reconstruct final atlas coverage with an area filter, then draw near 1:1.
- Pixel-snap text glyph X/Y positions on low-DPI output while preserving fractional placement on HiDPI displays.
- Normalize grayscale coverage for regular text glyphs so the embedded font does not look weak after reconstruction.

Implemented:
- Removed renderer-side platform font auto-detection from the default path.
- Removed unused internal `ca_font_detect_system()` discovery helper.
- Added `display_scale` to the font object so output pixel snapping is separate from glyph rasterization.
- Regular text glyphs now use `FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT` consistently across platforms.
- Low-DPI regular text glyphs render from 3x FreeType samples and are down-filtered into final atlas coverage by Causality.
- Supersampling is size-adaptive: small 1x text uses 3x, larger 1x text uses 2x, HiDPI uses native scale.
- Regular grayscale glyph coverage is strengthened only for small low-DPI text after area reconstruction; 14px+ code/input text avoids the heavier boost that made it chunky.
- Font atlas sampling now uses nearest coverage sampling because reconstructed atlas texels already contain final antialiasing; this avoids an extra GPU blur pass on 1x Windows displays.
- Low-DPI text glyph X positions are snapped to output pixels while HiDPI positioning remains fractional.
- Replaced the embedded Departure Mono font with Roboto Mono Nerd Font Mono Regular/Bold from `C:\Users\Duke\Downloads\RobotoMono`.
- Updated embedded font metadata and vendor notice to point to the Roboto Mono Apache-2.0 source and the Nerd Fonts patcher.

Verification:
- Plain `cmake --build build --config Debug` fails in a non-developer shell because MSVC include paths are not initialized and `<stdbool.h>` is not found.
- `cmd /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake --build build --config Debug'` passes.
- `cmd /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && ctest --test-dir build --output-on-failure'` runs successfully, but this build tree reports no registered tests.
- Directly running all `bin/*tests.exe` passes.
