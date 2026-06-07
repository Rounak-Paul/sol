// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* context_menu.c — Causality-backed context menus for Sol surfaces.
 *
 * This module owns menu construction and target metadata only. It does
 * not mutate buffers or the filesystem; selected items are reported as
 * typed SolUIContextActionRequest values to the application layer.
 */

#include "sol_ui_internal.h"

#include <stdlib.h>
#include <string.h>

static bool sol_ui_context_copy_path(SolContextMenuCtx *ctx, const char *path)
{
    if (!ctx) {
        return false;
    }
    ctx->path[0] = '\0';
    ctx->request.path = NULL;
    if (!path || path[0] == '\0') {
        return true;
    }
    const size_t len = strlen(path);
    if (len >= sizeof(ctx->path)) {
        return false;
    }
    memcpy(ctx->path, path, len + 1u);
    ctx->request.path = ctx->path;
    return true;
}

SolContextMenuCtx *sol_ui_acquire_context_menu_ctx(SolUISystem *ui)
{
    if (!ui) {
        return NULL;
    }

    if (ui->context_menu_ctx_count == ui->context_menu_ctx_capacity) {
        size_t new_cap = ui->context_menu_ctx_capacity
                            ? ui->context_menu_ctx_capacity * 2u
                            : 64u;
        SolContextMenuCtx **grown = (SolContextMenuCtx **)realloc(
            ui->context_menu_ctxs, new_cap * sizeof(SolContextMenuCtx *));
        if (!grown) {
            return NULL;
        }
        for (size_t i = ui->context_menu_ctx_capacity; i < new_cap; ++i) {
            grown[i] = NULL;
        }
        ui->context_menu_ctxs = grown;
        ui->context_menu_ctx_capacity = new_cap;
    }

    SolContextMenuCtx *ctx = ui->context_menu_ctxs[ui->context_menu_ctx_count];
    if (!ctx) {
        ctx = (SolContextMenuCtx *)calloc(1u, sizeof(SolContextMenuCtx));
        if (!ctx) {
            return NULL;
        }
        ui->context_menu_ctxs[ui->context_menu_ctx_count] = ctx;
    }
    ui->context_menu_ctx_count++;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ui = ui;
    return ctx;
}

void sol_ui_reset_context_menu_ctxs(SolUISystem *ui)
{
    if (ui) {
        ui->context_menu_ctx_count = 0u;
    }
}

static void sol_ui_on_context_select(int item_index, void *user_data)
{
    SolContextMenuCtx *ctx = (SolContextMenuCtx *)user_data;
    if (!ctx || !ctx->ui || item_index < 0 || item_index >= ctx->action_count) {
        return;
    }

    SolUIContextActionRequest request = ctx->request;
    request.action = ctx->actions[item_index];
    if (request.action == SOL_UI_CONTEXT_ACTION_NONE ||
        !ctx->ui->context_action_callback) {
        return;
    }
    (void)ctx->ui->context_action_callback(
        &request, ctx->ui->context_action_user_data);
}

static void sol_ui_on_context_open(float local_x, float local_y,
                                   float screen_x, float screen_y,
                                   void *user_data)
{
    SolContextMenuCtx *ctx = (SolContextMenuCtx *)user_data;
    if (!ctx) {
        return;
    }
    ctx->request.has_local_point = true;
    ctx->request.local_x = local_x;
    ctx->request.local_y = local_y;
    ctx->request.screen_x = screen_x;
    ctx->request.screen_y = screen_y;
}

static void sol_ui_attach_context_menu(SolContextMenuCtx *ctx,
                                       const char **labels,
                                       const SolUIContextAction *actions,
                                       int count)
{
    if (!ctx || !labels || !actions || count <= 0) {
        return;
    }
    if (count > (int)SOL_UI_MAX_CONTEXT_ACTIONS) {
        count = (int)SOL_UI_MAX_CONTEXT_ACTIONS;
    }
    for (int i = 0; i < count; ++i) {
        ctx->actions[i] = actions[i];
    }
    ctx->action_count = count;

    ca_context_menu(&(Ca_CtxMenuDesc){
        .items       = labels,
        .item_count  = count,
        .on_select   = sol_ui_on_context_select,
        .select_data = ctx,
        .on_open     = sol_ui_on_context_open,
        .open_data   = ctx,
    });
}

static SolContextMenuCtx *sol_ui_make_context(SolUISystem *ui,
                                              SolUIContextSurface surface,
                                              const char *path,
                                              bool path_is_dir,
                                              SolBufferNodeId leaf_id,
                                              SolBufferId buffer_id)
{
    SolContextMenuCtx *ctx = sol_ui_acquire_context_menu_ctx(ui);
    if (!ctx || !sol_ui_context_copy_path(ctx, path)) {
        return NULL;
    }
    ctx->request.surface = surface;
    ctx->request.path_is_dir = path_is_dir;
    ctx->request.leaf_id = leaf_id;
    ctx->request.buffer_id = buffer_id;
    return ctx;
}

void sol_ui_attach_workspace_context_menu(SolUISystem *ui)
{
    static const char *labels[] = {
        "New Buffer",
        "Open File...",
        "Open Folder...",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_NEW_BUFFER,
        SOL_UI_CONTEXT_ACTION_OPEN_FILE_PICKER,
        SOL_UI_CONTEXT_ACTION_OPEN_FOLDER_PICKER,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_WORKSPACE, NULL, false, 0u, 0u);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}

void sol_ui_attach_explorer_root_context_menu(SolUISystem *ui, const char *root_path)
{
    static const char *labels[] = {
        "New File",
        "New Folder",
        "Paste",
        "Open Folder...",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_NEW_FILE,
        SOL_UI_CONTEXT_ACTION_NEW_FOLDER,
        SOL_UI_CONTEXT_ACTION_PASTE_PATH,
        SOL_UI_CONTEXT_ACTION_OPEN_FOLDER_PICKER,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_EXPLORER_ROOT, root_path, true, 0u, 0u);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}

void sol_ui_attach_explorer_empty_context_menu(SolUISystem *ui, const char *root_path)
{
    static const char *labels[] = {
        "New File",
        "New Folder",
        "Paste",
        "Open Folder...",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_NEW_FILE,
        SOL_UI_CONTEXT_ACTION_NEW_FOLDER,
        SOL_UI_CONTEXT_ACTION_PASTE_PATH,
        SOL_UI_CONTEXT_ACTION_OPEN_FOLDER_PICKER,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_EXPLORER_EMPTY, root_path, true, 0u, 0u);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}

void sol_ui_attach_explorer_item_context_menu(SolUISystem *ui,
                                              const char *path,
                                              bool is_dir)
{
    static const char *file_labels[] = {
        "Open",
        "Copy",
        "Cut",
        "Paste",
        "New File",
        "New Folder",
        "Delete",
    };
    static const SolUIContextAction file_actions[] = {
        SOL_UI_CONTEXT_ACTION_OPEN,
        SOL_UI_CONTEXT_ACTION_COPY_PATH,
        SOL_UI_CONTEXT_ACTION_CUT_PATH,
        SOL_UI_CONTEXT_ACTION_PASTE_PATH,
        SOL_UI_CONTEXT_ACTION_NEW_FILE,
        SOL_UI_CONTEXT_ACTION_NEW_FOLDER,
        SOL_UI_CONTEXT_ACTION_DELETE_PATH,
    };
    static const char *dir_labels[] = {
        "Open",
        "New File",
        "New Folder",
        "Paste",
        "Copy",
        "Cut",
        "Delete",
    };
    static const SolUIContextAction dir_actions[] = {
        SOL_UI_CONTEXT_ACTION_OPEN,
        SOL_UI_CONTEXT_ACTION_NEW_FILE,
        SOL_UI_CONTEXT_ACTION_NEW_FOLDER,
        SOL_UI_CONTEXT_ACTION_PASTE_PATH,
        SOL_UI_CONTEXT_ACTION_COPY_PATH,
        SOL_UI_CONTEXT_ACTION_CUT_PATH,
        SOL_UI_CONTEXT_ACTION_DELETE_PATH,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_EXPLORER_ITEM, path, is_dir, 0u, 0u);
    if (is_dir) {
        sol_ui_attach_context_menu(ctx, dir_labels, dir_actions,
            (int)(sizeof(dir_actions) / sizeof(dir_actions[0])));
    } else {
        sol_ui_attach_context_menu(ctx, file_labels, file_actions,
            (int)(sizeof(file_actions) / sizeof(file_actions[0])));
    }
}

void sol_ui_attach_buffer_body_context_menu(SolUISystem *ui,
                                            SolBufferNodeId leaf_id,
                                            SolBufferId buffer_id)
{
    static const char *labels[] = {
        "Paste",
        "Select All",
        "New Buffer",
        "Close Buffer",
        "Split Vertical",
        "Split Horizontal",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_PASTE_TEXT,
        SOL_UI_CONTEXT_ACTION_SELECT_ALL_TEXT,
        SOL_UI_CONTEXT_ACTION_NEW_BUFFER,
        SOL_UI_CONTEXT_ACTION_CLOSE_BUFFER,
        SOL_UI_CONTEXT_ACTION_SPLIT_VERTICAL,
        SOL_UI_CONTEXT_ACTION_SPLIT_HORIZONTAL,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_BUFFER_BODY, NULL, false, leaf_id, buffer_id);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}

void sol_ui_system_attach_buffer_text_context_menu(SolUISystem *ui,
                                                   SolBufferNodeId leaf_id,
                                                   SolBufferId buffer_id)
{
    static const char *labels[] = {
        "Cut",
        "Copy",
        "Copy Line",
        "Paste",
        "Paste Line",
        "Select All",
        "Delete",
        "Delete Line",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_CUT_TEXT,
        SOL_UI_CONTEXT_ACTION_COPY_TEXT,
        SOL_UI_CONTEXT_ACTION_COPY_LINE,
        SOL_UI_CONTEXT_ACTION_PASTE_TEXT,
        SOL_UI_CONTEXT_ACTION_PASTE_LINE,
        SOL_UI_CONTEXT_ACTION_SELECT_ALL_TEXT,
        SOL_UI_CONTEXT_ACTION_DELETE_TEXT,
        SOL_UI_CONTEXT_ACTION_DELETE_LINE,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_BUFFER_TEXT, NULL, false, leaf_id, buffer_id);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}

void sol_ui_attach_buffer_tab_context_menu(SolUISystem *ui,
                                           SolBufferNodeId leaf_id,
                                           SolBufferId buffer_id)
{
    static const char *labels[] = {
        "Close Buffer",
        "New Buffer",
        "Split Vertical",
        "Split Horizontal",
    };
    static const SolUIContextAction actions[] = {
        SOL_UI_CONTEXT_ACTION_CLOSE_BUFFER,
        SOL_UI_CONTEXT_ACTION_NEW_BUFFER,
        SOL_UI_CONTEXT_ACTION_SPLIT_VERTICAL,
        SOL_UI_CONTEXT_ACTION_SPLIT_HORIZONTAL,
    };
    SolContextMenuCtx *ctx = sol_ui_make_context(
        ui, SOL_UI_CONTEXT_SURFACE_BUFFER_TAB, NULL, false, leaf_id, buffer_id);
    sol_ui_attach_context_menu(ctx, labels, actions,
                               (int)(sizeof(actions) / sizeof(actions[0])));
}
