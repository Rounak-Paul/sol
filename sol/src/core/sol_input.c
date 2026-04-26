// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_input.h"

SolModifierMask sol_modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if (mods & 0x0001) out |= SOL_MOD_SHIFT;
    if (mods & 0x0002) out |= SOL_MOD_CTRL;
    if (mods & 0x0004) out |= SOL_MOD_ALT;
    if (mods & 0x0008) out |= SOL_MOD_SUPER;
    return out;
}
