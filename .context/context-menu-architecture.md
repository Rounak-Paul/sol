Task context: implement Sol-wide context menus through Causality.

Existing runtime facts:
- Causality already exposes `ca_context_menu`, attached to the previously created element and opened on right-click.
- When several context menus overlap, Causality opens the smallest matching target, so row/text menus can sit inside broader empty-space menus.
- Causality context menus provide label plus selected index; Sol needs a typed action layer above this instead of decoding labels in app code.
- Existing text edit commands live in `main.c` behind `SOL_EVENT_COMMAND_INVOKED` semantics: copy, cut, paste, paste line, select all, delete char/word/line, undo/redo.
- The file tree owns visible entry paths and self-notifies through `sig_file_tree_rev`; filesystem mutations refresh the mounted explorer root through the UI root setter today.

Design:
- Add a Sol UI context callback API that reports `SolUIContextActionRequest` with action, surface, path, leaf, and buffer ids.
- Centralize menu item definitions and select dispatch in a new UI context-menu module.
- Attach menus to file-tree rows, tree root/empty space, buffer body, buffer text content, tab strip items, and welcome/workspace empty surfaces.
- Keep editor text operations in the existing command path by publishing typed context actions from UI and mapping them to the same logic in app code.
- Add platform helpers for create/delete/copy/move file operations so explorer context actions stay outside UI rendering code.

Implemented notes:
- `sol/src/ui/context_menu.c` owns menu labels, target metadata, and select-index dispatch.
- `main.c` maps context requests either to existing command events or to platform filesystem helpers.
- Deletes go through Causality confirmation popups before recursive removal.
- File copy/cut/paste is an editor-local file clipboard, separate from text clipboard content.

Hover fix:
- Context-menu hover highlight is drawn as a Causality paint overlay from the current mouse coordinates, not from a real child node.
- Normal `hovered_node` CSS dirty handling does not fire for this overlay, so moving inside an already-open menu did not repaint until some unrelated render happened.
- Causality now tracks `Ca_CtxMenu.hover_index` during `ca_widget_input_pass`; changes mark the bound target node content-dirty so the overlay repaints in the same frame.

Buffer text action fix:
- Text context actions need the right-clicked text location, not only the active leaf/buffer id; otherwise paste/delete can run at a stale cursor and copy/cut can silently do nothing without a selection.
- Causality context menus now emit an `on_open` callback with target-local and screen coordinates. Sol stores those coordinates on `SolUIContextActionRequest`.
- `text_view.c` owns conversion from text-view-local coordinates to buffer line/codepoint column, keeping rendering geometry out of app command dispatch.
- Main preserves an existing selection for selection-aware actions. With no selection, Copy/Cut act on the right-clicked line, Paste inserts at the right-clicked point, and Delete targets the right-clicked point/line as appropriate.
