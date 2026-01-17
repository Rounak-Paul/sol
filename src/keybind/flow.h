/*
 * flow.h - Vim-like command flow system
 *
 * Leader key + key sequences for commands.
 * Supports count prefixes (e.g., "4x" for 4 undos).
 * Leader key: Ctrl+Space (activates command flow mode)
 *
 * Flow commands:
 *   o - open file
 *   O - open folder  
 *   x - undo (with count: 4x = undo 4 times)
 *   X - undo all
 *   r - redo (with count)
 *   R - redo all
 *   s - save
 *   S - save all
 *   w - close buffer
 *   W - close all buffers
 *   c - copy
 *   v - paste
 *   q - quit
 *   f - find
 *   n - new file
 *   / - search
 *   p - command palette
 */

#ifndef SOL_FLOW_H
#define SOL_FLOW_H

#include "../sol.h"

/* Check if key activates leader (Ctrl+Space) */
bool sol_flow_is_leader(sol_keychord* chord);

/* Handle key input in flow mode 
 * Returns true if key was consumed */
bool sol_flow_handle_key(sol_editor* ed, sol_keychord* chord);

/* Cancel flow mode */
void sol_flow_cancel(sol_editor* ed);

/* Get flow mode status string (for status bar) */
void sol_flow_status_string(sol_editor* ed, char* buf, size_t len);

#endif /* SOL_FLOW_H */
