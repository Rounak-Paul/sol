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
