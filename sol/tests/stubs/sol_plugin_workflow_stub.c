// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* Link the plugin unit tests without mounting a workspace or spawning a PTY. */

#include "sol_ui_system.h"
#include "sol_terminal.h"

SolUIToolbarToken sol_ui_system_register_toolbar(SolUISystem *ui,
                                                 const SolUIToolbarDesc *desc)
{
    (void)ui; (void)desc;
    return 1u;
}

void sol_ui_system_unregister_toolbar(SolUISystem *ui, SolUIToolbarToken token)
{
    (void)ui; (void)token;
}

void sol_ui_system_refresh_project_tabs(SolUISystem *ui) { (void)ui; }

SolTerminal *sol_terminal_manager_new_tab(SolTerminalManager *mgr, const char *cwd)
{
    (void)mgr; (void)cwd;
    return NULL;
}

void sol_terminal_manager_set_visible(SolTerminalManager *mgr, bool visible)
{
    (void)mgr; (void)visible;
}

void sol_ui_system_terminal_set_focused(SolUISystem *ui, bool focused)
{
    (void)ui; (void)focused;
}

void sol_ui_system_terminal_notify(SolUISystem *ui) { (void)ui; }

void sol_terminal_send_text(SolTerminal *term, const char *data, size_t len)
{
    (void)term; (void)data; (void)len;
}
