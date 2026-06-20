# CSS System — CSS3 Implementation

## Overview

Full CSS3-level implementation across both repos:
- Standalone: `/Users/duke/Code/causality/`
- Submodule: `/Users/duke/Code/sol/vendors/causality/`

Both build cleanly as of the last session.

## Property Coverage (~75 properties)

### Box Model
- `width`, `height`, `min-width`, `max-width`, `min-height`, `max-height`
- `padding`, `padding-{top|right|bottom|left}`
- `margin`, `margin-{top|right|bottom|left}`
- `gap`, `row-gap`, `column-gap`
- `box-sizing` (content-box / border-box)
- `aspect-ratio`

### Flexbox
- `display` (flex / block / none / inline / inline-flex / grid)
- `flex-direction`, `flex-wrap`, `flex-flow`
- `flex-grow`, `flex-shrink`, `flex-basis`, `flex` (shorthand)
- `align-items`, `align-self`, `align-content`
- `justify-content`, `justify-self`
- `place-items`, `place-content`
- `order`

### Typography
- `font-size`, `font-weight`, `font-style`
- `line-height`, `letter-spacing`, `word-spacing`
- `text-align`, `text-decoration`, `text-transform`, `white-space`
- `text-wrap`

### Visual
- `color`, `background-color`, `background`
- `opacity`, `visibility`
- `border-radius`, `border-top-left-radius`, `border-top-right-radius`,
  `border-bottom-right-radius`, `border-bottom-left-radius`

### Borders
- Uniform: `border-width`, `border-color`, `border-style`, `border`
- Per-side: `border-{top|right|bottom|left}-{width|color|style}`
- Shorthands: `border-{top|right|bottom|left}`
- Outline: `outline-width`, `outline-color`, `outline-style`, `outline-offset`, `outline`

### Shadows / Z-index
- `box-shadow`, `z-index`

### Overflow / Scroll
- `overflow`, `overflow-x`, `overflow-y`, `scroll-behavior`

### Interaction
- `cursor`, `pointer-events`, `user-select`

### Animation
- `transition` (shorthand: property + duration)

## Color Functions
- `#rrggbb` / `#rgba` / `#rrggbbaa`
- `rgb()`, `rgba()`
- `hsl()`, `hsla()` — full HLS→RGB math
- `oklch()` — full OKLab matrix + gamma sRGB conversion
- `color-mix()` — weighted blend in sRGB
- 80+ named CSS colors
- `currentColor`, `inherit`, `initial`

## Length Units
- `px`, unitless number
- `%` (percentage)
- `em`, `rem` (resolved to 16px default)
- `vw`, `vh` (resolved to 1920×1080 default)
- `pt`, `pc`, `cm`, `mm`, `in` → px conversion
- `calc()`, `min()`, `max()`, `clamp()` — flat left-associative expression

## Selector Support
- Element, class (`.foo`), ID (`#bar`)
- Pseudo-classes: `:hover`, `:active`, `:focus`, `:focus-within`,
  `:disabled`, `:enabled`, `:checked`, `:root`, `:empty`,
  `:first-child`, `:last-child`, `:only-child`,
  `:first-of-type`, `:last-of-type`,
  `:nth-child(An+B)`, `:nth-last-child(An+B)`,
  `:not(simple-selector)`
- Combinators: descendant (ws), child (`>`), adjacent (`+`), general sibling (`~`)
- Pseudo-elements (`::before` etc.) — skipped
- Attribute selectors `[...]` — skipped

## Specificity
`(id_count << 20) | ((class_count + pseudo_count) << 10) | element_count`

## Two-Level set_mask
`Ca_ResolvedStyle` has two uint64_t fields:
- `set_mask`: tracks Ca_CssPropId values 0–63
- `set_mask2`: tracks values 64+
- `STYLE_SET(prop)` macro handles the split

## Cascade
Standard CSS cascade: specificity → source-order. In submodule: two-pass
(non-!important first, !important second) with resolve_value() for CSS variables.

## Submodule-Specific Features (preserved)
- `CA_CSS_VAL_VAR` and `var(--custom-property)` resolution
- `!important` tracking via `Ca_CssDecl.important` + `consume_important()`
- `Ca_CssVar` + string pool (`ca_css_intern`/`ca_css_str`)
- `width_pct` / `height_pct` on `Ca_NodeDesc` and `Ca_ResolvedStyle`
- `font_bold` bool alongside `font_weight` int
- Short per-side border names: `border_top_w/c` vs `border_top_width/color`
- `Ca_CssPseudo` array with `Ca_CssPseudoKind` enum (vs standalone bit-fields)
- `CA_CSS_MAX_RULES = 1024`

## Layout Extensions
- `row_gap`/`column_gap` → main/cross gap selection per axis
- `flex_basis` takes priority over width/height on main axis
- `align_self` per-child cross-axis override (0=inherit from parent's align_items)
- `aspect_ratio` derives one axis from the other when auto

## Paint Extensions
- `visibility_hidden` → early return in `paint_node_content`
- CSS `outline` → 4-rect rendering outside the border box
- `letter_spacing` → added to glyph advance in `paint_text`

## Known Renderer Limitations
- GPU rect shader only supports uniform border width (`Ca_RectPushConst`).
  Per-side borders are drawn as separate edge rects (CPU-side, correct).
- Per-corner border-radius: stored in `Ca_NodeDesc`, GPU uses `corner_radius`
  (max of four corners).

## Key Files
```
causality/src/ui/css.h          — property IDs, keyword enum, selector types
causality/src/ui/css.c          — tokenizer, parser, color funcs, shorthands
causality/src/ui/style.h        — Ca_ResolvedStyle, STYLE_SET macro
causality/src/ui/style.c        — cascade, selector matching, apply_to_node
causality/src/core/ca_internal.h— Ca_NodeDesc with all new fields
causality/src/ui/widget.c       — apply_css: px scaling at 3 sites
causality/src/ui/layout.c       — flex layout with new properties
causality/src/ui/paint.c        — visibility, outline, letter-spacing
```
