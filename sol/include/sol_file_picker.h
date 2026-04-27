// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_file_picker.h — Reusable native-window file/folder picker for Sol.
 *
 * Opens a separate causality window that lets the user navigate the
 * filesystem and pick either a file or a directory. On confirmation
 * the supplied callback fires with the absolute path; on cancel or
 * window close the callback is invoked with NULL.
 *
 * Boundaries:
 *   - No dependency on Sol's buffer system or UI internals. The only
 *     causality contact is to create the window and build its UI.
 *   - Pickers are owned by the module: the caller must NOT free the
 *     returned handle. Internal state is reaped automatically once
 *     the window is closed (by user or programmatically).
 *
 * Lifetime:
 *   - sol_file_picker_open creates and shows the window.
 *   - sol_file_picker_tick must be called from the host's per-frame
 *     hook so closed windows can be reaped. It is safe to call when
 *     no pickers are open.
 */

#ifndef SOL_FILE_PICKER_H
#define SOL_FILE_PICKER_H

#include <stdbool.h>

#include <causality.h>

typedef struct SolFilePicker SolFilePicker;

typedef enum SolFilePickerMode {
    SOL_FILE_PICKER_FILE   = 0,
    SOL_FILE_PICKER_FOLDER = 1,
} SolFilePickerMode;

/* Invoked exactly once per picker. `path` is the absolute selected
 * path on confirm, or NULL when the user cancels / closes the window. */
typedef void (*SolFilePickerCallback)(const char *path, void *user_data);

/* Create and show a picker window. Returns NULL on failure. The
 * returned handle is owned by the module — do not free it. */
SolFilePicker *sol_file_picker_open(Ca_Instance          *instance,
                                    SolFilePickerMode     mode,
                                    const char           *initial_dir,
                                    SolFilePickerCallback on_select,
                                    void                 *user_data);

/* Reap any pickers whose window has been closed. Safe to call every
 * frame from the primary window's on_frame hook. */
void sol_file_picker_tick(void);

#endif /* SOL_FILE_PICKER_H */
