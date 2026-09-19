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

Validation: `cmake --build build --target sol_text_buffer_tests sol --parallel
6`, `ctest --test-dir build --output-on-failure` (20/20), and `git diff --check`.
