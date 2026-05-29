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

/* Per-row indent in pixels. */
#define SOL_UI_TREE_INDENT_PX  14.0f

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

    /* Indentation lives on a leading spacer div so the button itself
       fills the rest of the row width — easier to make the click target
       cover the whole strip later if we want to. */
    float indent = SOL_UI_TREE_INDENT_PX * (float)entry->depth;

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "tree-row",
        .direction  = CA_HORIZONTAL,
        .on_click   = on_row_click,
        .click_data = ctx,
    });

    if (indent > 0.0f) {
        ca_div_begin(&(Ca_DivDesc){
            .width = indent,
            .style = "tree-indent",
        });
        ca_div_end();
    }

    /* Disclosure / kind glyph. Causality ships with Roboto Mono Nerd
       Font as the default, so we can use Material/FontAwesome glyphs
       directly in UTF-8 string literals. */
    const char *glyph;
    if (entry->is_dir)
        glyph = entry->expanded ? "\xef\x81\xb8"      /*  chevron-down  */
                                : "\xef\x81\xb4";     /*  chevron-right */
    else
        glyph = "\xef\x85\x9b";                       /*  file          */
    ca_text(&(Ca_TextDesc){
        .text  = glyph,
        .style = entry->is_dir ? "tree-glyph tree-glyph-dir" : "tree-glyph",
    });

    /* Folder body icon (only for dirs) sits between the chevron and the
       label — gives the row a clear "folder vs file" silhouette. */
    if (entry->is_dir) {
        ca_text(&(Ca_TextDesc){
            .text  = entry->expanded ? "\xef\x81\xbc"  /*  folder-open */
                                     : "\xef\x81\xbb", /*  folder      */
            .style = "tree-icon tree-icon-dir",
        });
    }

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

    /* Header: shows the root path's basename. */
    const char *root = sol_file_tree_root(ui->file_tree);
    const char *slash = strrchr(root, '/');
    const char *header = (slash && slash[1] != '\0') ? slash + 1 : root;
    ca_text(&(Ca_TextDesc){
        .text  = header,
        .style = "tree-header",
    });

    /* Rows. */
    size_t count = sol_file_tree_visible_count(ui->file_tree);
    for (size_t i = 0; i < count; ++i) {
        const SolFileEntry *entry = sol_file_tree_visible(ui->file_tree, i);
        if (entry) render_row(ui, entry, i);
    }
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
