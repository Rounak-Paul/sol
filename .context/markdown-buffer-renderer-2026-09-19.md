# In-buffer Markdown renderer

Markdown files render inline from their existing `SolTextBuffer` rope; no
second buffer or derived document is created, so unsaved edits and file-watcher
behaviour remain owned by the normal text-buffer lifecycle. The cursor line is
the unmodified normal editor row, including its ordinary caret, selection,
pointer positioning, pane geometry, and cursor-settling behaviour. Other
visible rows substitute Markdown presentation without changing row geometry.

`sol_markdown` is the document boundary: it provides stateless inline tokens
(emphasis, strong, strike-through, inline code, links) and a stream block
parser (headings, quotes, lists/tasks, rules, tables, fenced code). Fenced
blocks retain their language identifier, which is the correct future dispatch
point for Mermaid, Plotly, and other embedded renderers. The text view contains
only Causality presentation mapping; it no longer parses Markdown itself.

Table rows render as individual cells with consistent grid backgrounds; table
alignment separators render as dividers. Fenced-code rows add a full-width
background layer before rendering source text, so the block is visually unified
without affecting selection, caret positioning, or horizontal scrolling.

Table layout makes a table-wide measurement pass over contiguous rows. Every
column uses its Causality-measured widest rendered cell, plus one character
column on the left and two on the right, with no minimum or maximum. All rows and
separator cells consume that exact schema. The viewport derives
its horizontal extent from rendered table width, so long values remain
single-line and use the normal buffer horizontal scrollbar instead of clipping.
It never relies on flex content measurement, so long values cannot shift
columns or make table separators drift.

Tables use a distinct header, alternating body rows, subtle column dividers,
and an accent separator. The default palette is blue; curated themes override
all table tones from their semantic primary, surface, elevated, text, and
secondary colors.

The full rendered Markdown vocabulary is theme-owned: headings, paragraphs,
emphasis, links, quotes, inline and fenced code, rules, lists, and tasks use
the same semantic colors as the normal buffer and syntax highlighter.

Validation: `cmake --build build --target sol_text_buffer_tests sol --parallel
6`, `ctest --test-dir build --output-on-failure` (20/20), and `git diff --check`.
