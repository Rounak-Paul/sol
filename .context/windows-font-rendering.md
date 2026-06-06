# Windows Font Rendering Context

Task: improve Causality/Sol font rendering on Windows while preserving macOS quality.

Relevant paths:
- `vendors/causality/causality/src/renderer/font.c`: FreeType-backed dynamic glyph atlas, system font detection, glyph load/render flags.
- `vendors/causality/causality/src/renderer/font.h`: glyph quad generation and baked atlas metrics.
- `vendors/causality/causality/src/ui/paint.c`: text draw command generation and glyph positioning.
- `sol/src/main.c`: creates `Ca_Instance` without a font override, so renderer defaults matter.

Findings:
- The app normally uses embedded `DepartureMonoNerdFontMono-Regular.otf`.
- macOS looks better largely because content scale is commonly 2x, so grayscale glyphs have more physical pixels.
- Current glyph load flags force auto-hinting/light hinting for regular text. On Windows, native TrueType bytecode hinting is usually better for installed Windows fonts.
- Current paint code keeps X glyph positions fractional and the atlas sampler is linear. This is fine on HiDPI but makes grayscale glyphs soft/uneven on standard-density Windows displays.
- Causality should not default to platform fonts; the desired product direction is deterministic embedded-font rendering across Windows, macOS, Linux, and screen densities.

Change direction:
- Keep the embedded Causality font bundle as the default on every platform.
- Use one controlled FreeType path for regular text everywhere.
- Internally rasterize glyphs at at least 2x source resolution, even on 1x displays, then draw them at logical size.
- Pixel-snap text glyph X/Y positions on low-DPI output while preserving fractional placement on HiDPI displays.
- Normalize grayscale coverage for regular text glyphs so the embedded font does not look weak after 2x downsampling.

Implemented:
- Removed renderer-side platform font auto-detection from the default path.
- Removed unused internal `ca_font_detect_system()` discovery helper.
- Added `display_scale` and minimum `content_scale`/raster scale to the font object.
- Regular text glyphs now use `FT_LOAD_TARGET_NORMAL | FT_LOAD_FORCE_AUTOHINT` consistently across platforms.
- Regular grayscale glyph coverage is strengthened deterministically during atlas blit.
- Low-DPI text glyph X positions are snapped to output pixels while glyphs still sample from the higher-resolution atlas.

Verification:
- `cmake --build build --config Debug` currently fails before project code is compiled because MSVC cannot find `<stdbool.h>` from `causality.h`. This appears to be a local compiler/include-path environment issue, not a patch-specific compiler error.
