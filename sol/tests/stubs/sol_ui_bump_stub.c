// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ui_bump_stub.c — Provides the sol_ui_bump_u32 helper without
 * pulling in the full workspace.c compilation unit.
 *
 * For tests only. workspace.c defines the real version.
 */

#include "sol_ui_internal.h"

void sol_ui_bump_u32(Ca_Signal *sig)
{
    if (!sig) return;
    ca_signal_set_u32(sig, ca_signal_get_u32(sig) + 1u);
}
