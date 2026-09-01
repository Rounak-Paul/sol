// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ssh_config.h — Saved SSH connection profiles.
 *
 * Sol stores remote-connection profiles under the same per-user
 * directory as every other setting:
 *
 *     $HOME/.sol/ssh_connections.json
 *
 * A profile records enough to reconnect (host, port, user, auth
 * method, key path) but deliberately NEVER a password — a
 * SOL_SSH_AUTH_PASSWORD profile always re-prompts at connect time.
 * Storing a plaintext password on disk next to bindings.conf would
 * turn a config-file leak into a credential leak; every other editor
 * that persists SSH credentials (VS Code Remote, JetBrains Gateway)
 * makes the same call for the same reason.
 *
 * File shape (array of profiles, unlike settings.json's single
 * top-level object — connections are inherently a list):
 *
 *     [
 *       { "name": "dev-box", "host": "dev.example.com", "port": 22,
 *         "user": "duke", "auth": "key",
 *         "key_path": "~/.ssh/id_ed25519" },
 *       { "name": "prod",    "host": "10.0.0.4",         "port": 2222,
 *         "user": "deploy",  "auth": "agent" }
 *     ]
 */

#ifndef SOL_SSH_CONFIG_H
#define SOL_SSH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOL_SSH_CONNECTION_MAX   64u
#define SOL_SSH_NAME_MAX         64u
#define SOL_SSH_HOST_MAX         256u
#define SOL_SSH_USER_MAX         64u
#define SOL_SSH_PATH_MAX         1024u

typedef enum SolSshAuthMethod {
    SOL_SSH_AUTH_PASSWORD = 0,  /* prompted at connect time, never persisted */
    SOL_SSH_AUTH_KEY      = 1,  /* private key file, optional passphrase prompt */
    SOL_SSH_AUTH_AGENT    = 2,  /* delegate to a running ssh-agent (SSH_AUTH_SOCK) */
} SolSshAuthMethod;

typedef struct SolSshConnection {
    char             name[SOL_SSH_NAME_MAX];       /* display label, e.g. "dev-box" */
    char             host[SOL_SSH_HOST_MAX];
    uint16_t         port;                         /* 0 in a loaded profile means "use 22" */
    char             user[SOL_SSH_USER_MAX];
    SolSshAuthMethod auth;
    char             key_path[SOL_SSH_PATH_MAX];   /* only meaningful for SOL_SSH_AUTH_KEY */
} SolSshConnection;

typedef struct SolSshConnectionList {
    SolSshConnection items[SOL_SSH_CONNECTION_MAX];
    size_t           count;
} SolSshConnectionList;

/*
 * Load saved connection profiles from ~/.sol/ssh_connections.json.
 *
 * Missing file is not an error — out is populated with count == 0, the
 * same "first run, nothing saved yet" convention as sol_settings_load.
 * Per-entry parse errors are skipped (log to stderr) rather than
 * aborting the whole load, so one malformed entry doesn't hide every
 * other saved connection.
 *
 * out  Receives the parsed list.
 * Returns  true unless the file exists but could not be read at all
 *          (a missing file still returns true with out->count == 0).
 */
bool sol_ssh_config_load(SolSshConnectionList *out);

/*
 * Persist the given connection list to ~/.sol/ssh_connections.json.
 *
 * list  The connections to persist.
 * Returns  true on success.
 */
bool sol_ssh_config_save(const SolSshConnectionList *list);

/*
 * Append or update (by name) one connection profile in-place, then
 * save the whole list.
 *
 * Convenience wrapper around load → upsert → save for the "Save
 * connection" action in the connect dialog, so callers never need to
 * hand-roll the load/find/save sequence themselves.
 *
 * conn  The profile to save. conn->name identifies it; an existing
 *       entry with the same name is overwritten, otherwise a new one
 *       is appended (dropped silently if the list is already at
 *       SOL_SSH_CONNECTION_MAX capacity).
 * Returns  true on success.
 */
bool sol_ssh_config_upsert(const SolSshConnection *conn);

/*
 * Remove one connection profile (by name) and save the resulting list.
 *
 * name  Profile name to remove.
 * Returns  true if a matching profile was found and removed and the
 *          list was saved; false if no profile had that name.
 */
bool sol_ssh_config_remove(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SSH_CONFIG_H */
