// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* file_tree_panel.c — Left-side file hierarchy panel.
 *
 * Renders the SolFileTree's visible projection as a vertical list of
 * clickable rows. Directories toggle on click; files are routed to the
 * UI system's registered file_open_callback.
 *
 * Click contexts are pooled on SolUISystem and grown on demand so the
 * pointers we hand to causality buttons stay valid across frame rebuilds.
 */

#include "sol_ui_internal.h"

#include "sol_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ASCII case-insensitive string comparison.
 *
 * Returns Negative, zero, or positive like strcmp.
 */
static int sol_ascii_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a;
        int cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Width of each indent level in px — matches .tree-indent flex-shrink:0 width
   set programmatically via Ca_DivDesc.width. */
#define SOL_UI_TREE_INDENT_PX  16.0f

#define TREE_ARROW_RIGHT  CA_ICON_FA_CHEVRON_RIGHT
#define TREE_ARROW_DOWN   CA_ICON_FA_CHEVRON_DOWN
#define TREE_DIR_CLOSED   CA_ICON_FA_FOLDER
#define TREE_DIR_OPEN     CA_ICON_FA_FOLDER_OPEN
#define TREE_FILE_COG     CA_ICON_FA_COG
#define TREE_FILE_GENERIC CA_ICON_FA_FILE_O
#define TREE_FILE_C_LANG  CA_ICON_NF_DEV_C
#define TREE_FILE_CPP     CA_ICON_NF_DEV_CPP
#define TREE_FILE_PYTHON  CA_ICON_NF_DEV_PYTHON
#define TREE_FILE_JS      CA_ICON_NF_DEV_JAVASCRIPT
#define TREE_FILE_TS      CA_ICON_NF_SETI_TYPESCRIPT
#define TREE_FILE_HTML    CA_ICON_NF_DEV_HTML5
#define TREE_FILE_CSS     CA_ICON_NF_DEV_CSS3
#define TREE_FILE_JSON    CA_ICON_NF_DEV_JSON
#define TREE_FILE_MD      CA_ICON_NF_FA_MARKDOWN

/* Pick the right icon glyph and CSS class for a file based on extension. */
static const char *tree_file_icon(const char *name, const char **out_style)
{
    if (name && (strncmp(name, "CMakeLists", 10) == 0)) {
        *out_style = "tree-icon tree-icon-cmake"; return TREE_FILE_COG;
    }
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot) {
        /* C family */
        if (sol_ascii_casecmp(dot, ".c")     == 0) { *out_style = "tree-icon tree-icon-c";    return TREE_FILE_C_LANG; }
        if (sol_ascii_casecmp(dot, ".h")     == 0) { *out_style = "tree-icon tree-icon-h";    return TREE_FILE_C_LANG; }
        if (sol_ascii_casecmp(dot, ".cpp")   == 0 ||
            sol_ascii_casecmp(dot, ".cc")    == 0) { *out_style = "tree-icon tree-icon-c";    return TREE_FILE_CPP;    }
        if (sol_ascii_casecmp(dot, ".hpp")   == 0 ||
            sol_ascii_casecmp(dot, ".hh")    == 0) { *out_style = "tree-icon tree-icon-h";    return TREE_FILE_CPP;    }
        /* Scripting */
        if (sol_ascii_casecmp(dot, ".py")    == 0) { *out_style = "tree-icon tree-icon-py";   return TREE_FILE_PYTHON; }
        if (sol_ascii_casecmp(dot, ".js")    == 0) { *out_style = "tree-icon tree-icon-js";   return TREE_FILE_JS;     }
        if (sol_ascii_casecmp(dot, ".ts")    == 0) { *out_style = "tree-icon tree-icon-ts";   return TREE_FILE_TS;     }
        /* Data / config */
        if (sol_ascii_casecmp(dot, ".json")  == 0) { *out_style = "tree-icon tree-icon-json"; return TREE_FILE_JSON;   }
        /* Web */
        if (sol_ascii_casecmp(dot, ".html")  == 0 ||
            sol_ascii_casecmp(dot, ".htm")   == 0) { *out_style = "tree-icon tree-icon-html"; return TREE_FILE_HTML;   }
        if (sol_ascii_casecmp(dot, ".css")   == 0) { *out_style = "tree-icon tree-icon-css";  return TREE_FILE_CSS;    }
        /* Prose */
        if (sol_ascii_casecmp(dot, ".md")    == 0) { *out_style = "tree-icon tree-icon-md";   return TREE_FILE_MD;     }
        /* Build */
        if (sol_ascii_casecmp(dot, ".cmake") == 0) { *out_style = "tree-icon tree-icon-cmake";return TREE_FILE_COG;    }
    }
    *out_style = "tree-icon tree-icon-file";
    return TREE_FILE_GENERIC;
}

/* ---------------------------------------------------------------- */
/* Public setters                                                    */
/* ---------------------------------------------------------------- */

/*
 * Set the file tree root to the given filesystem path and update explorer
 * visibility: shows the panel when path is non-NULL, hides it otherwise.
 *
 * ui    The UI system whose file tree is updated.
 * path  Absolute path of the new root directory (NULL clears the tree).
 * Returns true on success, false when the UI system or tree is unavailable.
 */
bool sol_ui_system_set_file_tree_root(SolUISystem *ui, const char *path)
{
    if (!ui || !ui->file_tree) return false;
    /* Root changes self-notify via sig_file_tree_rev; visibility is a
       separate UI concern managed by the workspace builder. */
    const bool ok = sol_file_tree_set_root(ui->file_tree, path);
    if (ok && path) {
        sol_ui_system_set_file_tree_visible(ui, true);
    } else {
        sol_ui_system_set_file_tree_visible(ui, false);
    }
    return ok;
}

/*
 * Register the callback invoked when a file is opened from the tree panel.
 *
 * ui         The UI system to configure.
 * callback   Function called with the selected file path.
 * user_data  Opaque pointer forwarded to callback.
 */
void sol_ui_system_set_file_open_callback(SolUISystem *ui,
                                          SolUIFileOpenFn callback,
                                          void *user_data)
{
    if (!ui) return;
    ui->file_open_callback = callback;
    ui->file_open_user_data = user_data;
}

/* ---------------------------------------------------------------- */
/* Click ctx pool                                                    */
/* ---------------------------------------------------------------- */

/*
 * Return a pointer to the next available file-tree click context from the
 * pool, growing it with realloc when necessary.
 *
 * ui      The UI system owning the pool.
 * Returns Pointer to the next free context, or NULL on allocation failure.
 */
static SolFileTreeClickCtx *acquire_click_ctx(SolUISystem *ui)
{
    if (ui->file_tree_click_ctx_count == ui->file_tree_click_ctx_capacity) {
        size_t new_cap = ui->file_tree_click_ctx_capacity
                            ? ui->file_tree_click_ctx_capacity * 2u
                            : 64u;
        SolFileTreeClickCtx *grown = (SolFileTreeClickCtx *)realloc(
            ui->file_tree_click_ctxs, new_cap * sizeof(SolFileTreeClickCtx));
        if (!grown) return NULL;
        ui->file_tree_click_ctxs = grown;
        ui->file_tree_click_ctx_capacity = new_cap;
    }
    return &ui->file_tree_click_ctxs[ui->file_tree_click_ctx_count++];
}

/* ---------------------------------------------------------------- */
/* Click handlers                                                    */
/* ---------------------------------------------------------------- */

/*
 * Handle a click on a file-tree row.  Toggles directory expansion or invokes
 * the file-open callback depending on the entry type.  Also notifies the
 * focus-region callback so the editor panel loses focus.
 */
static void on_row_click(Ca_Button *button, void *user_data)
{
    (void)button;
    SolFileTreeClickCtx *ctx = (SolFileTreeClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->file_tree) return;

    if (ctx->ui->focus_region_callback) {
        ctx->ui->focus_region_callback(true, ctx->ui->focus_region_user_data);
    }

    const SolFileEntry *entry =
        sol_file_tree_visible(ctx->ui->file_tree, ctx->row_index);
    if (!entry) return;

    if (entry->is_dir) {
        /* sol_file_tree_toggle self-notifies via sig_file_tree_rev. */
        (void)sol_file_tree_toggle(ctx->ui->file_tree, ctx->row_index);
    } else if (ctx->ui->file_open_callback) {
        /* The callback typically creates/switches a buffer, which
           self-notifies via sig_buffer_rev — nothing else to do. */
        ctx->ui->file_open_callback(entry->full_path,
                                    ctx->ui->file_open_user_data);
    }
}

/* ---------------------------------------------------------------- */
/* Rendering                                                         */
/* ---------------------------------------------------------------- */

/*
 * Emit the four-slot Causality button for one file-tree row: indent spacer,
 * disclosure arrow, file/dir icon, and name label.
 *
 * ui         The UI system (provides pool and callbacks).
 * entry      The file entry to render.
 * row_index  The entry's index in the visible list (stored in click context).
 */
static void render_row(SolUISystem *ui, const SolFileEntry *entry,
                       size_t row_index)
{
    SolFileTreeClickCtx *ctx = acquire_click_ctx(ui);
    if (!ctx) return;
    ctx->ui = ui;
    ctx->row_index = row_index;

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "tree-row",
        .direction  = CA_HORIZONTAL,
        .on_click   = on_row_click,
        .click_data = ctx,
    });

    /* Slot 1 — Indent spacer.
       Always a single div; width scales with depth. Using ONE div (not N
       separate guide divs) keeps the child count identical for every row,
       which prevents causality from recycling stale widgets during tree
       expansion and causing phantom glyphs on adjacent rows. */
    ca_div_begin(&(Ca_DivDesc){
        .style = "tree-indent",
        .width = (float)entry->depth * SOL_UI_TREE_INDENT_PX,
    });
    ca_div_end();

    /* Slot 2 — Disclosure arrow.
       Always a ca_text so the slot type never changes. Files get an empty
       string so no character is drawn but the width is held by CSS. */
    const char *arrow_text  = "";
    const char *arrow_style = "tree-arrow";
    if (entry->is_dir) {
        arrow_text  = entry->expanded ? TREE_ARROW_DOWN : TREE_ARROW_RIGHT;
        arrow_style = entry->expanded ? "tree-arrow tree-arrow-open" : "tree-arrow";
    }
    ca_text(&(Ca_TextDesc){ .text = arrow_text, .style = arrow_style });

    /* Slot 3 — Icon. Always a ca_text. */
    const char *icon_style = NULL;
    const char *icon;
    if (entry->is_dir) {
        icon       = entry->expanded ? TREE_DIR_OPEN    : TREE_DIR_CLOSED;
        icon_style = entry->expanded ? "tree-icon tree-icon-dir-open"
                                     : "tree-icon tree-icon-dir-closed";
    } else {
        icon = tree_file_icon(entry->name, &icon_style);
    }
    ca_text(&(Ca_TextDesc){ .text = icon, .style = icon_style });

    /* Slot 4 — Name. Always a ca_text. */
    ca_text(&(Ca_TextDesc){
        .text  = entry->name,
        .style = entry->is_dir ? "tree-name tree-name-dir" : "tree-name",
    });

    ca_btn_end();
    sol_ui_attach_explorer_item_context_menu(ui, entry->full_path, entry->is_dir);
}

/* Emits header + rows directly into the current widget context. The
   caller is responsible for opening a styled "tree-panel" container.
   Used as the body of the reactive tree-panel sub-builder. */
/*
 * Emit the file-tree panel content (header, root row, and scrollable list)
 * directly into the current widget context.  Pre-sizes the click-context pool
 * before the render loop to prevent mid-loop realloc from invalidating
 * distributed pointers.
 *
 * ui  The UI system providing the file tree and context-menu callbacks.
 */
void sol_ui_render_file_tree_panel_body(SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_ui_system_file_tree_visible(ui) ||
        !sol_file_tree_root(ui->file_tree)) return;

    /* Pre-size the click-context pool before the render loop.
       acquire_click_ctx calls realloc when the array needs to grow.  If
       that realloc moves the backing store, every pointer already
       distributed to a button's click_data earlier in THIS loop becomes
       a dangling pointer — the crash in on_row_click at the ctx->ui
       check.  Allocating enough capacity up-front guarantees realloc
       never runs mid-loop, keeping all distributed pointers stable. */
    size_t needed = sol_file_tree_visible_count(ui->file_tree);
    if (needed > ui->file_tree_click_ctx_capacity) {
        size_t new_cap = ui->file_tree_click_ctx_capacity
                             ? ui->file_tree_click_ctx_capacity * 2u
                             : 64u;
        while (new_cap < needed) new_cap *= 2u;
        SolFileTreeClickCtx *grown = (SolFileTreeClickCtx *)realloc(
            ui->file_tree_click_ctxs, new_cap * sizeof(SolFileTreeClickCtx));
        if (grown) {
            ui->file_tree_click_ctxs = grown;
            ui->file_tree_click_ctx_capacity = new_cap;
        }
    }

    /* Reset the click-context pool for this rebuild. */
    ui->file_tree_click_ctx_count = 0u;

    /* Root basename for the header */
    const char *root = sol_file_tree_root(ui->file_tree);
    const char *basename = sol_platform_basename(root);
    if (!basename || basename[0] == '\0') {
        basename = root;
    }
    (void)basename; /* used in header label below */

    /* ---- Sticky section header ---- */
    ca_div_begin(&(Ca_DivDesc){ .style = "tree-section-header" });
    ca_text(&(Ca_TextDesc){ .text = "EXPLORER", .style = "tree-section-title" });
    ca_div_end();

    /* ---- Root folder row (static label, not interactive).
       Deliberately omits the disclosure arrow — there is nothing to
       collapse at the root level, and showing one creates a false
       affordance. The user closes the panel via explorer.focus.toggle. ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "tree-root-row",
    });
    ca_text(&(Ca_TextDesc){ .text = TREE_DIR_OPEN, .style = "tree-icon tree-icon-dir-open" });
    ca_text(&(Ca_TextDesc){ .text = basename,       .style = "tree-name tree-name-dir" });
    ca_div_end();
    sol_ui_attach_explorer_root_context_menu(ui, root);

    /* ---- Scrollable file list ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "tree-scroll-area",
        .id        = "tree-list",
    });

    size_t count = sol_file_tree_visible_count(ui->file_tree);
    for (size_t i = 0; i < count; ++i) {
        const SolFileEntry *entry = sol_file_tree_visible(ui->file_tree, i);
        if (entry) render_row(ui, entry, i);
    }

    ca_div_end(); /* tree-scroll-area */
    sol_ui_attach_explorer_empty_context_menu(ui, root);
}

/* Standalone variant: opens its own "tree-panel" container. Kept for
   any caller that wants to drop a one-shot panel into a non-reactive
   context (e.g. a dialog). The reactive workspace path uses the body
   variant directly. */
/*
 * Standalone variant of the file-tree panel renderer that wraps content in
 * its own "tree-panel" container div.  Suitable for non-reactive one-shot
 * contexts; the reactive workspace path calls the body variant directly.
 *
 * ui  The UI system providing the file tree.
 */
void sol_ui_render_file_tree_panel(SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_ui_system_file_tree_visible(ui) ||
        !sol_file_tree_root(ui->file_tree)) return;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "tree-panel",
    });
    sol_ui_render_file_tree_panel_body(ui);
    ca_div_end();
}

/* ---------------------------------------------------------------- */
/* Sticky ancestor headers                                          */
/* ---------------------------------------------------------------- */

/* Walk the visible list backward from first_row to collect the ancestor
   directory of each depth level.  Returns the count of ancestors found
   in top-down order (shallowest first).  Writes at most max_out entries
   into *out_entries[] and the corresponding visible-list index into
   *out_indices[]. */
/*
 * Walk the visible tree list backward from the first visible row to collect
 * ancestor directory entries for the sticky-header overlay.
 *
 * tree         The file tree to query.
 * scroll_y     Current scroll offset in logical pixels.
 * out_entries  Output array filled with ancestor entry pointers, top-down.
 * out_indices  Output array filled with the visible-list index for each
 *              ancestor entry, parallel to out_entries.
 * max_out      Maximum number of ancestors to collect.
 * Returns      Number of ancestors found and written (0 when at the top).
 */
static int find_sticky_ancestors(SolFileTree *tree, float scroll_y,
                                 const SolFileEntry **out_entries,
                                 size_t *out_indices,
                                 int max_out)
{
    size_t count = sol_file_tree_visible_count(tree);
    if (count == 0 || max_out <= 0) return 0;

    int first_row = (int)(scroll_y / SOL_UI_TREE_ROW_H);
    if (first_row <= 0) return 0;                      /* at top, nothing sticky */
    if (first_row >= (int)count) first_row = (int)count - 1;

    int target_depth = (int)sol_file_tree_visible(tree, (size_t)first_row)->depth - 1;
    if (target_depth < 0) return 0;                    /* depth-0 row, no parents */

    /* Walk backward collecting one ancestor per depth level. */
    const SolFileEntry *ancestors[32];
    size_t              ancestor_idx[32];
    int found = 0;
    for (int i = first_row - 1; i >= 0 && target_depth >= 0; i--) {
        const SolFileEntry *e = sol_file_tree_visible(tree, (size_t)i);
        if (!e) continue;
        if (e->is_dir && (int)e->depth == target_depth) {
            ancestors[found]    = e;
            ancestor_idx[found] = (size_t)i;
            ++found;
            --target_depth;
            if (found >= max_out || found >= 32) break;
        }
    }

    /* ancestors[] is collected bottom-up; reverse to top-down order. */
    for (int i = 0; i < found; i++) {
        out_entries[i] = ancestors[found - 1 - i];
        out_indices[i] = ancestor_idx[found - 1 - i];
    }
    return found;
}

/* Scroll the tree list to the row position stored in the sticky click context. */
static void on_sticky_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolStickyClickCtx *ctx = (SolStickyClickCtx *)user_data;
    if (!ctx) return;
    ca_set_scroll_y(ctx->window, "tree-list", ctx->target_scroll_y);
}

/* Render one sticky-header row — same four-child structure as render_row
   (indent + arrow + icon + name) using a button for click-to-scroll. */
/*
 * Emit a sticky-header row button with the same four-slot layout as a normal
 * row (indent, arrow, icon, name).  Clicking scrolls the tree list to reveal
 * the directory that owns this header.
 *
 * entry  The ancestor directory entry to render.
 * ctx    Click context carrying the target scroll position.
 */
static void render_sticky_row(const SolFileEntry *entry, SolStickyClickCtx *ctx)
{
    ca_btn_begin(&(Ca_BtnDesc){
        .direction  = CA_HORIZONTAL,
        .style      = "tree-sticky-row",
        .on_click   = on_sticky_click,
        .click_data = ctx,
    });
    /* 1. depth indent */
    ca_div_begin(&(Ca_DivDesc){
        .style = "tree-indent",
        .width = (float)entry->depth * SOL_UI_TREE_INDENT_PX,
    });
    ca_div_end();
    /* 2. arrow — always "down" since ancestor is expanded */
    ca_text(&(Ca_TextDesc){ .text = TREE_ARROW_DOWN, .style = "tree-arrow tree-arrow-open" });
    /* 3. icon — open folder */
    ca_text(&(Ca_TextDesc){ .text = TREE_DIR_OPEN, .style = "tree-icon tree-icon-dir-open" });
    /* 4. name */
    ca_text(&(Ca_TextDesc){ .text = entry->name, .style = "tree-name tree-name-dir" });
    ca_btn_end();
}

/* Reactive builder installed on tree_sticky_host.
   Subscribes to sig_tree_scroll (fires on every scroll tick) and
   sig_file_tree_rev (fires on tree structure change).  Only the sticky
   overlay re-runs on scroll — the main workspace content is unaffected. */
/*
 * Reactive builder installed on the tree sticky-header host div.  Subscribes
 * to sig_tree_scroll and sig_file_tree_rev so only the sticky overlay reruns
 * on scroll events; the main workspace content remains unaffected.
 *
 * div        The sticky-header host div (unused directly).
 * user_data  Pointer to the SolUISystem.
 */
void sol_ui_sticky_tree_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->file_tree || !ui->sig_tree_scroll || !ui->sig_file_tree_rev) return;

    float scroll_y = ca_signal_get_float(ui->sig_tree_scroll);
    (void)ca_signal_get_u32(ui->sig_file_tree_rev);

    if (!sol_ui_system_file_tree_visible(ui) ||
        !sol_file_tree_root(ui->file_tree)) return;

    ui->sticky_click_ctx_count = 0;

    const SolFileEntry *ancestors[SOL_UI_TREE_STICKY_MAX];
    size_t              indices[SOL_UI_TREE_STICKY_MAX];
    int n = find_sticky_ancestors(ui->file_tree, scroll_y,
                                  ancestors, indices, SOL_UI_TREE_STICKY_MAX);
    for (int i = 0; i < n; i++) {
        SolStickyClickCtx *ctx = &ui->sticky_click_ctxs[ui->sticky_click_ctx_count++];
        ctx->window          = ui->primary_window;
        ctx->target_scroll_y = (float)indices[i] * SOL_UI_TREE_ROW_H;
        render_sticky_row(ancestors[i], ctx);
    }
}
