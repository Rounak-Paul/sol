// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ssh_window.h — SSH connect dialog.
 *
 * A single-window dialog for entering or picking a saved SSH
 * connection profile, then handing the result back to the caller to
 * actually open (as a terminal channel, and in a later phase, the
 * remote workspace).
 *
 * Layout (per sol_ssh_window.c's header comment): a left column
 * listing saved connections (sol_ssh_config.h), and a right form with
 * Name/Host/Port/User, an auth-method dropdown, the auth-method's
 * matching field (key-file picker or password), and
 * Connect/Save/Cancel actions.
 *
 * Boundaries, matching sol_file_picker.h's model:
 *   - No dependency on Sol's buffer/workspace internals — only touches
 *     Causality window/widget calls and sol_ssh_config.h.
 *   - Owned by the module: the caller must not free the returned handle.
 *   - sol_ui_ssh_window_tick must run once per frame so a closed window
 *     is reaped.
 */

#ifndef SOL_SSH_WINDOW_H
#define SOL_SSH_WINDOW_H

#include <causality.h>

#include "sol_ssh_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolSshConnectWindow SolSshConnectWindow;

/*
 * Invoked once when the user clicks Connect with a valid profile filled
 * in. Never invoked on Cancel or a plain window close.
 *
 * conn       The filled-in connection profile (name/host/port/user/auth).
 *            key_path is set for SOL_SSH_AUTH_KEY; password is the raw
 *            typed value for SOL_SSH_AUTH_PASSWORD (empty for the other
 *            two methods) — passed out-of-band from conn since
 *            SolSshConnection itself is never allowed to carry one (see
 *            sol_ssh_config.h).
 * password   Password typed for this connection attempt, or "" when
 *            conn->auth != SOL_SSH_AUTH_PASSWORD. Valid only for the
 *            duration of the callback — copy it if it must outlive the
 *            call.
 * user_data  The user_data pointer supplied to sol_ui_ssh_window_open.
 */
typedef void (*SolSshConnectCallback)(const SolSshConnection *conn,
                                      const char             *password,
                                      void                   *user_data);

/*
 * Open the SSH connect dialog. No-op if one is already open (same
 * singleton convention as sol_ui_settings_window_open).
 *
 * instance   Causality instance used to create the window.
 * on_connect Called once when the user confirms a connection attempt.
 * user_data  Passed unchanged to on_connect.
 */
void sol_ui_ssh_window_open(Ca_Instance *instance,
                            SolSshConnectCallback on_connect,
                            void *user_data);

/*
 * Reap the connect window once it has been closed. Call once per frame
 * from the host's frame hook, alongside sol_file_picker_tick /
 * sol_ui_settings_window_tick. Safe to call when no window is open.
 */
void sol_ui_ssh_window_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SSH_WINDOW_H */
