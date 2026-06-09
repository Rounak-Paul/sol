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

/* Controls whether the picker selects a file or a directory. */
typedef enum SolFilePickerMode {
    SOL_FILE_PICKER_FILE   = 0,
    SOL_FILE_PICKER_FOLDER = 1,
} SolFilePickerMode;

/*
 * Invoked exactly once per picker when the user confirms or cancels.
 *
 * path       Absolute selected path on confirm, or NULL on cancel/close.
 * user_data  The user_data pointer supplied to sol_file_picker_open.
 */
typedef void (*SolFilePickerCallback)(const char *path, void *user_data);

/*
 * Create and show a file or folder picker window.
 *
 * instance    The causality instance used to create the window.
 * mode        Whether to pick a file or a directory.
 * initial_dir Starting directory shown in the picker, or NULL for cwd.
 * on_select   Callback invoked once with the selected path (or NULL on cancel).
 * user_data   Passed unchanged to on_select.
 * Returns     An opaque handle owned by the module (do not free), or NULL on failure.
 */
SolFilePicker *sol_file_picker_open(Ca_Instance          *instance,
                                    SolFilePickerMode     mode,
                                    const char           *initial_dir,
                                    SolFilePickerCallback on_select,
                                    void                 *user_data);

/*
 * Reap any pickers whose window has been closed.
 *
 * Call this once per frame from the primary window's on_frame hook.
 * Safe to call when no pickers are open.
 */
void sol_file_picker_tick(void);

#endif /* SOL_FILE_PICKER_H */
