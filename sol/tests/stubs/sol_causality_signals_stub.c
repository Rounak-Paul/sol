// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_causality_signals_stub.c
 *
 * Minimal stub for causality signal API used by sol_buffer.c.
 * Tests never call sol_buffer_attach_revision_signal(), so system->rev
 * is always NULL and these symbols are never actually invoked.  They
 * must exist at link time to satisfy the linker without pulling in the
 * full causality shared library (and its Vulkan dependency). */

#include <stdint.h>

/* Ca_Signal is opaque; forward-declare just enough for the ABI. */
typedef struct Ca_Signal Ca_Signal;

uint32_t ca_signal_get_u32(const Ca_Signal *sig) { (void)sig; return 0u; }
void     ca_signal_set_u32(Ca_Signal *sig, uint32_t value) { (void)sig; (void)value; }
