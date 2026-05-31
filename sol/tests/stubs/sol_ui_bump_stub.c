// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ui_bump_stub.c — Provides UI helpers without pulling in the full
 * workspace.c compilation unit.  For test binaries only.
 */

#include "sol_ui_internal.h"

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
