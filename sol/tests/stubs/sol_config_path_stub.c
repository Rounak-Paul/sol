// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_config_path_stub.c — config-path stub for tests that link
 * sol_settings.c only for its pure helpers (sol_settings_defaults,
 * sol_settings_build_appearance_css).
 *
 * The real sol_config_path lives in sol_config.c, which also holds the
 * key-bindings loader and therefore pulls in the whole UI system. Returning
 * NULL here makes settings load/save fail cleanly rather than touching the
 * developer's real ~/.sol/settings.json from a unit test.
 */

#include "sol_config.h"

#include <stddef.h>

char *sol_config_path(const char *filename)
{
    (void)filename;
    return NULL;
}
