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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */

/* Width of each indent level in px — matches .tree-indent flex-shrink:0 width
   set programmatically via Ca_DivDesc.width. */
#define SOL_UI_TREE_INDENT_PX  16.0f

/* FontAwesome 4 glyphs — all in the U+F000–U+F2FF range that Roboto Mono
   Nerd Font is guaranteed to include. Bytes computed as 3-byte UTF-8. */
#define TREE_ARROW_RIGHT  "\xef\x81\x94"   /* U+F054  fa-chevron-right   */
#define TREE_ARROW_DOWN   "\xef\x81\xb8"   /* U+F078  fa-chevron-down    */
#define TREE_DIR_CLOSED   "\xef\x81\xbb"   /* U+F07B  fa-folder          */
#define TREE_DIR_OPEN     "\xef\x81\xbc"   /* U+F07C  fa-folder-open     */
#define TREE_FILE_CODE    "\xef\x87\x89"   /* U+F1C9  fa-file-code-o     */
#define TREE_FILE_TEXT    "\xef\x83\xb6"   /* U+F0F6  fa-file-text-o     */
#define TREE_FILE_COG     "\xef\x80\x93"   /* U+F013  fa-cog (cmake)     */
#define TREE_FILE_GENERIC "\xef\x80\x96"   /* U+F016  fa-file-o          */

/* Pick the right icon glyph and CSS class for a file based on extension. */
static const char *tree_file_icon(const char *name, const char **out_style)
{
    if (name && (strncmp(name, "CMakeLists", 10) == 0)) {
        *out_style = "tree-icon tree-icon-cmake"; return TREE_FILE_COG;
    }
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot) {
        if (strcasecmp(dot, ".c")     == 0) { *out_style = "tree-icon tree-icon-c";     return TREE_FILE_CODE;    }
        if (strcasecmp(dot, ".h")     == 0) { *out_style = "tree-icon tree-icon-h";     return TREE_FILE_CODE;    }
        if (strcasecmp(dot, ".cpp")   == 0 ||
            strcasecmp(dot, ".cc")    == 0) { *out_style = "tree-icon tree-icon-c";     return TREE_FILE_CODE;    }
        if (strcasecmp(dot, ".md")    == 0) { *out_style = "tree-icon tree-icon-md";    return TREE_FILE_TEXT;    }
        if (strcasecmp(dot, ".cmake") == 0) { *out_style = "tree-icon tree-icon-cmake"; return TREE_FILE_COG;     }
    }
    *out_style = "tree-icon tree-icon-file";
    return TREE_FILE_GENERIC;
}

/* ---------------------------------------------------------------- */
/* Public setters                                                    */
/* ---------------------------------------------------------------- */

bool sol_ui_system_set_file_tree_root(SolUISystem *ui, const char *path)
{
    if (!ui || !ui->file_tree) return false;
    /* sol_file_tree_set_root self-notifies via sig_file_tree_rev, so
       the workspace content builder re-runs and the tree panel column
       appears/disappears as appropriate. */
    return sol_file_tree_set_root(ui->file_tree, path);
}

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
}

/* Emits header + rows directly into the current widget context. The
   caller is responsible for opening a styled "tree-panel" container.
   Used as the body of the reactive tree-panel sub-builder. */
void sol_ui_render_file_tree_panel_body(SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_file_tree_root(ui->file_tree)) return;

    /* Reset the click-context pool for this rebuild. */
    ui->file_tree_click_ctx_count = 0u;

    /* Root basename for the header */
    const char *root = sol_file_tree_root(ui->file_tree);
    const char *slash = strrchr(root, '/');
    const char *basename = (slash && slash[1] != '\0') ? slash + 1 : root;
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

    /* ---- Scrollable file list ---- */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "tree-scroll-area" });

    size_t count = sol_file_tree_visible_count(ui->file_tree);
    for (size_t i = 0; i < count; ++i) {
        const SolFileEntry *entry = sol_file_tree_visible(ui->file_tree, i);
        if (entry) render_row(ui, entry, i);
    }

    ca_div_end(); /* tree-scroll-area */
}

/* Standalone variant: opens its own "tree-panel" container. Kept for
   any caller that wants to drop a one-shot panel into a non-reactive
   context (e.g. a dialog). The reactive workspace path uses the body
   variant directly. */
void sol_ui_render_file_tree_panel(SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_file_tree_root(ui->file_tree)) return;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "tree-panel",
    });
    sol_ui_render_file_tree_panel_body(ui);
    ca_div_end();
}
