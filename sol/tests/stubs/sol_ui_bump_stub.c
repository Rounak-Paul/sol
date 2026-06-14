// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ui_bump_stub.c — Provides UI helpers without pulling in the full
 * workspace.c compilation unit.  For test binaries only.
 */

#include "sol_ui_internal.h"

#include <stdio.h>
#include <string.h>

void sol_ui_bump_u32(Ca_Signal *sig)
{
    if (!sig) return;
    ca_signal_set_u32(sig, ca_signal_get_u32(sig) + 1u);
}

/* workspace.c owns the real version; tests don't exercise plugin
   window functionality so a no-op stub is sufficient.              */
void sol_ui_system_set_plugin_manager(SolUISystem *ui, SolPluginManager *pm)
{
    (void)ui;
    (void)pm;
}

void sol_ui_system_open_plugin_window(SolUISystem *ui)
{
    (void)ui;
}

SolUIMenuItemToken sol_ui_system_register_menu_item(
    SolUISystem *ui,
    const SolUIMenuItemDesc *desc)
{
    if (!ui || !desc || !desc->menu_id || !desc->menu_label ||
        !desc->item_id || !desc->label || !desc->action) return 0u;
    for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
        if (!ui->menu_items[i].in_use) {
            SolUIMenuItem *item = &ui->menu_items[i];
            memset(item, 0, sizeof(*item));
            item->ui = ui;
            item->token = ++ui->menu_item_next_token;
            if (item->token == 0u) item->token = ++ui->menu_item_next_token;
            snprintf(item->menu_id, sizeof(item->menu_id), "%s", desc->menu_id);
            snprintf(item->menu_label, sizeof(item->menu_label), "%s", desc->menu_label);
            snprintf(item->item_id, sizeof(item->item_id), "%s", desc->item_id);
            snprintf(item->label, sizeof(item->label), "%s", desc->label);
            snprintf(item->action, sizeof(item->action), "%s", desc->action);
            snprintf(item->submenu_id, sizeof(item->submenu_id), "%s",
                     desc->submenu_id ? desc->submenu_id : "");
            snprintf(item->submenu_label, sizeof(item->submenu_label), "%s",
                     desc->submenu_label ? desc->submenu_label : "");
            item->in_use = true;
            return item->token;
        }
    }
    return 0u;
}

void sol_ui_system_unregister_menu_item(SolUISystem *ui,
                                        SolUIMenuItemToken token)
{
    if (!ui || token == 0u) return;
    for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
        if (ui->menu_items[i].in_use && ui->menu_items[i].token == token) {
            memset(&ui->menu_items[i], 0, sizeof(ui->menu_items[i]));
            return;
        }
    }
}

SolUISidePanelToken sol_ui_system_register_side_panel(
    SolUISystem *ui,
    const SolUISidePanelDesc *desc)
{
    if (!ui || !desc || !desc->id || !desc->render) return 0u;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        if (!ui->side_panels[i].in_use) {
            SolUISidePanel *panel = &ui->side_panels[i];
            memset(panel, 0, sizeof(*panel));
            panel->token = ++ui->side_panel_next_token;
            if (panel->token == 0u) panel->token = ++ui->side_panel_next_token;
            snprintf(panel->id, sizeof(panel->id), "%s", desc->id);
            panel->render = desc->render;
            panel->tick = desc->tick;
            panel->user_data = desc->user_data;
            panel->in_use = true;
            return panel->token;
        }
    }
    return 0u;
}

void sol_ui_system_unregister_side_panel(SolUISystem *ui,
                                         SolUISidePanelToken token)
{
    if (!ui || token == 0u) return;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        if (ui->side_panels[i].in_use && ui->side_panels[i].token == token) {
            memset(&ui->side_panels[i], 0, sizeof(ui->side_panels[i]));
            if (ui->active_side_panel == token) ui->active_side_panel = 0u;
            return;
        }
    }
}

bool sol_ui_system_show_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token)
{
    if (!ui || token == 0u) return false;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        if (ui->side_panels[i].in_use && ui->side_panels[i].token == token) {
            ui->active_side_panel = token;
            return true;
        }
    }
    return false;
}

void sol_ui_system_hide_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token)
{
    if (ui && ui->active_side_panel == token) ui->active_side_panel = 0u;
}

bool sol_ui_system_side_panel_visible(const SolUISystem *ui,
                                      SolUISidePanelToken token)
{
    return ui && token != 0u && ui->active_side_panel == token;
}

void sol_ui_system_notify_side_panel(SolUISystem *ui,
                                     SolUISidePanelToken token)
{
    (void)ui;
    (void)token;
}

void sol_ui_system_wake(SolUISystem *ui)
{
    (void)ui;
}
