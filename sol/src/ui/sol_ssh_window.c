// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ssh_window.c — SSH connect dialog.
 *
 * Two-panel layout (620 × 420), matching settings_window.c's structure:
 *
 *   ┌── ssh-left (200px) ──┐  ┌── ssh-right (flex-grow) ────────────┐
 *   │  SAVED               │  │  Name    [___________]              │
 *   │  ▸ dev-box        [x]│  │  Host    [___________]  Port [___]  │
 *   │  ▸ prod           [x]│  │  User    [___________]              │
 *   │                      │  │  Auth    [Password ▾]               │
 *   │  [ + New ]           │  │  Password [***********]             │
 *   │                      │  │                                     │
 *   │                      │  │        [Save]  [Cancel]  [Connect]  │
 *   └──────────────────────┘  └─────────────────────────────────────┘
 *
 * Clicking a saved connection loads it into the form (password field
 * stays empty — passwords are never persisted, see sol_ssh_config.h).
 * Save writes the current form to ~/.sol/ssh_connections.json without
 * connecting. Connect fires the caller's callback with the current form
 * contents (persisting first only if the user also clicked Save).
 *
 * Follows the same open-once singleton pattern as settings_window.c and
 * plugin_window.c.
 */

#include "sol_ui_internal.h"
#include "sol_file_picker.h"
#include "sol_ssh_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSHW_DEFAULT_WIDTH   620
#define SSHW_DEFAULT_HEIGHT  420

#define SSHW_AUTH_OPTION_COUNT 3
static const char * const SSHW_AUTH_OPTIONS[SSHW_AUTH_OPTION_COUNT] = {
    "Password", "Private key", "SSH agent",
};

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

struct SolSshConnectWindow {
    Ca_Window   *window;
    Ca_Instance *instance;
    Ca_Signal   *sig_rev;
    Ca_Div      *content_host;

    SolSshConnectCallback on_connect;
    void                  *on_connect_data;

    /* Saved-connections list, refreshed from disk each time the window
       opens and after every Save/Delete — no live file-watch, matching
       how settings.json/bindings.conf are only re-read at those same
       explicit moments elsewhere in Sol. */
    SolSshConnectionList saved;

    /* Form fields — edited in place, independent of `saved` until the
       user explicitly clicks a saved entry (loads it) or Save (writes
       the form's current contents back into `saved`). */
    char     name[SOL_SSH_NAME_MAX];
    char     host[SOL_SSH_HOST_MAX];
    char     port_text[8];
    char     user[SOL_SSH_USER_MAX];
    char     key_path[SOL_SSH_PATH_MAX];
    char     password[256];
    int      auth_selected;   /* index into SSHW_AUTH_OPTIONS */

    char     status_text[128];   /* validation/save feedback, cleared on edit */

    SolFilePicker *key_picker;   /* non-NULL while the Browse dialog is open */
};

/* ------------------------------------------------------------------ */
/* Singleton list (same convention as settings_window.c)               */
/* ------------------------------------------------------------------ */

static SolSshConnectWindow *g_sshw = NULL;

/* ------------------------------------------------------------------ */
/* Form <-> SolSshConnection conversions                               */
/* ------------------------------------------------------------------ */

static SolSshAuthMethod sshw_auth_from_index(int idx)
{
    switch (idx) {
    case 1:  return SOL_SSH_AUTH_KEY;
    case 2:  return SOL_SSH_AUTH_AGENT;
    default: return SOL_SSH_AUTH_PASSWORD;
    }
}

static int sshw_index_from_auth(SolSshAuthMethod auth)
{
    switch (auth) {
    case SOL_SSH_AUTH_KEY:   return 1;
    case SOL_SSH_AUTH_AGENT: return 2;
    default:                 return 0;
    }
}

/* Loads one saved connection's fields into the form. Password is
   deliberately left untouched (cleared by the caller beforehand) —
   password-auth profiles never carry one on disk to load. */
static void sshw_load_into_form(SolSshConnectWindow *w, const SolSshConnection *c)
{
    snprintf(w->name, sizeof(w->name), "%s", c->name);
    snprintf(w->host, sizeof(w->host), "%s", c->host);
    snprintf(w->port_text, sizeof(w->port_text), "%u", (unsigned)c->port);
    snprintf(w->user, sizeof(w->user), "%s", c->user);
    snprintf(w->key_path, sizeof(w->key_path), "%s", c->key_path);
    w->auth_selected = sshw_index_from_auth(c->auth);
}

/* Builds a SolSshConnection from the current form contents.
   Returns false (and sets status_text) when a required field is
   missing, without mutating *out. */
static bool sshw_form_to_connection(SolSshConnectWindow *w, SolSshConnection *out)
{
    if (w->host[0] == '\0') {
        snprintf(w->status_text, sizeof(w->status_text), "Host is required.");
        return false;
    }
    if (w->name[0] == '\0') {
        snprintf(w->status_text, sizeof(w->status_text), "Name is required.");
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", w->name);
    snprintf(out->host, sizeof(out->host), "%s", w->host);
    snprintf(out->user, sizeof(out->user), "%s", w->user);
    out->auth = sshw_auth_from_index(w->auth_selected);
    if (out->auth == SOL_SSH_AUTH_KEY)
        snprintf(out->key_path, sizeof(out->key_path), "%s", w->key_path);

    long port = strtol(w->port_text, NULL, 10);
    out->port = (port > 0 && port <= 65535) ? (uint16_t)port : 22u;
    return true;
}

/* ------------------------------------------------------------------ */
/* Callbacks — text/select fields                                      */
/* ------------------------------------------------------------------ */

#define SSHW_ON_TEXT_CHANGE(field_name, field, max_len)                    \
    static void sshw_on_##field_name##_change(Ca_TextInput *inp, void *ud) \
    {                                                                       \
        SolSshConnectWindow *w = (SolSshConnectWindow *)ud;                \
        snprintf(w->field, (max_len), "%s", ca_input_text(inp));           \
        w->status_text[0] = '\0';                                          \
    }

SSHW_ON_TEXT_CHANGE(name,     name,      sizeof(w->name))
SSHW_ON_TEXT_CHANGE(host,     host,      sizeof(w->host))
SSHW_ON_TEXT_CHANGE(port,     port_text, sizeof(w->port_text))
SSHW_ON_TEXT_CHANGE(user,     user,      sizeof(w->user))
SSHW_ON_TEXT_CHANGE(key_path, key_path,  sizeof(w->key_path))
SSHW_ON_TEXT_CHANGE(password, password,  sizeof(w->password))

#undef SSHW_ON_TEXT_CHANGE

static void sshw_on_auth_change(Ca_Select *sel, void *user_data)
{
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    int idx = ca_select_get(sel);
    if (idx < 0 || idx >= SSHW_AUTH_OPTION_COUNT) return;
    w->auth_selected = idx;
    w->status_text[0] = '\0';
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Callbacks — saved-connections list                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    SolSshConnectWindow *win;
    size_t                index;
} SshwSavedCtx;

/* Allocated once per rebuild (list size is small and bounded by
   SOL_SSH_CONNECTION_MAX) and freed on the next rebuild — same
   short-lived-context idiom used for tab contexts in settings_window.c,
   except heap-allocated here since the count is dynamic. */
static SshwSavedCtx *g_sshw_saved_ctxs = NULL;
static size_t         g_sshw_saved_ctx_count = 0u;

static void sshw_on_saved_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SshwSavedCtx *ctx = (SshwSavedCtx *)user_data;
    SolSshConnectWindow *w = ctx->win;
    if (ctx->index >= w->saved.count) return;
    w->password[0] = '\0';
    sshw_load_into_form(w, &w->saved.items[ctx->index]);
    w->status_text[0] = '\0';
    sol_ui_bump_u32(w->sig_rev);
}

static void sshw_on_saved_delete(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SshwSavedCtx *ctx = (SshwSavedCtx *)user_data;
    SolSshConnectWindow *w = ctx->win;
    if (ctx->index >= w->saved.count) return;
    sol_ssh_config_remove(w->saved.items[ctx->index].name);
    sol_ssh_config_load(&w->saved);
    sol_ui_bump_u32(w->sig_rev);
}

static void sshw_on_new_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    memset(w->name, 0, sizeof(w->name));
    memset(w->host, 0, sizeof(w->host));
    memset(w->user, 0, sizeof(w->user));
    memset(w->key_path, 0, sizeof(w->key_path));
    memset(w->password, 0, sizeof(w->password));
    snprintf(w->port_text, sizeof(w->port_text), "22");
    w->auth_selected = 0;
    w->status_text[0] = '\0';
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Callbacks — key-file picker                                         */
/* ------------------------------------------------------------------ */

static void sshw_on_key_picked(const char *path, void *user_data)
{
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    w->key_picker = NULL;
    if (!path) return;   /* cancelled */
    snprintf(w->key_path, sizeof(w->key_path), "%s", path);
    sol_ui_bump_u32(w->sig_rev);
}

static void sshw_on_browse_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    if (w->key_picker) return;   /* already open */
    w->key_picker = sol_file_picker_open(w->instance, SOL_FILE_PICKER_FILE,
                                         NULL, sshw_on_key_picked, w);
}

/* ------------------------------------------------------------------ */
/* Callbacks — actions                                                 */
/* ------------------------------------------------------------------ */

static void sshw_on_save_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    SolSshConnection conn;
    if (!sshw_form_to_connection(w, &conn)) {
        sol_ui_bump_u32(w->sig_rev);
        return;
    }
    if (sol_ssh_config_upsert(&conn)) {
        sol_ssh_config_load(&w->saved);
        snprintf(w->status_text, sizeof(w->status_text), "Saved \"%s\".", conn.name);
    } else {
        snprintf(w->status_text, sizeof(w->status_text), "Could not save connection.");
    }
    sol_ui_bump_u32(w->sig_rev);
}

static void sshw_on_cancel_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    if (w->window) ca_window_close(w->window);
}

static void sshw_on_connect_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    SolSshConnection conn;
    if (!sshw_form_to_connection(w, &conn)) {
        sol_ui_bump_u32(w->sig_rev);
        return;
    }
    if (conn.auth == SOL_SSH_AUTH_PASSWORD && w->password[0] == '\0') {
        snprintf(w->status_text, sizeof(w->status_text), "Password is required.");
        sol_ui_bump_u32(w->sig_rev);
        return;
    }

    SolSshConnectCallback cb = w->on_connect;
    void *cb_data = w->on_connect_data;
    const char *pw = (conn.auth == SOL_SSH_AUTH_PASSWORD) ? w->password : "";

    if (w->window) ca_window_close(w->window);
    /* Fire after requesting the window close (not after it's actually
       reaped — sol_ui_ssh_window_tick handles that next frame) so the
       callback can freely open further UI (e.g. a "connecting..."
       terminal tab) without this window's now-stale state getting in
       the way. w itself is still valid here; only w->window transitions
       to "closing." */
    if (cb) cb(&conn, pw, cb_data);
}

/* ------------------------------------------------------------------ */
/* Content builder                                                     */
/* ------------------------------------------------------------------ */

static void sshw_render_saved_list(SolSshConnectWindow *w)
{
    ca_text(&(Ca_TextDesc){ .text = "SAVED", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    free(g_sshw_saved_ctxs);
    g_sshw_saved_ctxs = (w->saved.count > 0u)
        ? (SshwSavedCtx *)calloc(w->saved.count, sizeof(SshwSavedCtx))
        : NULL;
    g_sshw_saved_ctx_count = w->saved.count;

    for (size_t i = 0u; i < w->saved.count; ++i) {
        if (g_sshw_saved_ctxs) {
            g_sshw_saved_ctxs[i].win   = w;
            g_sshw_saved_ctxs[i].index = i;
        }

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL, .style = "sw-tab-btn",
        });
        ca_btn_begin(&(Ca_BtnDesc){
            .direction  = CA_HORIZONTAL,
            .style      = "sw-tab-btn",
            .on_click   = sshw_on_saved_click,
            .click_data = g_sshw_saved_ctxs ? &g_sshw_saved_ctxs[i] : NULL,
        });
        ca_text(&(Ca_TextDesc){
            .text = w->saved.items[i].name, .style = "sw-tab-label",
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "x",
            .style      = "pm-btn-disable",
            .on_click   = sshw_on_saved_delete,
            .click_data = g_sshw_saved_ctxs ? &g_sshw_saved_ctxs[i] : NULL,
        });
        ca_btn_end();
        ca_div_end();
    }

    ca_btn_begin(&(Ca_BtnDesc){
        .text = "+ New", .style = "welcome-btn",
        .on_click = sshw_on_new_click, .click_data = w,
    });
    ca_btn_end();
}

static void sshw_render_form(SolSshConnectWindow *w)
{
    const SolSshAuthMethod auth = sshw_auth_from_index(w->auth_selected);

#define SSHW_INPUT_ROW(row_label, field, hint, cb)                      \
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" }); \
    ca_text(&(Ca_TextDesc){ .text = (row_label), .style = "sw-setting-label" }); \
    ca_input(&(Ca_InputDesc){                                           \
        .text = w->field, .placeholder = (hint),                        \
        .on_change = (cb), .change_data = w, .style = "search-input",   \
    });                                                                  \
    ca_div_end();

    SSHW_INPUT_ROW("Name", name, "e.g. dev-box", sshw_on_name_change)
    SSHW_INPUT_ROW("Host", host, "hostname or IP", sshw_on_host_change)
    SSHW_INPUT_ROW("Port", port_text, "22", sshw_on_port_change)
    SSHW_INPUT_ROW("User", user, "username", sshw_on_user_change)

#undef SSHW_INPUT_ROW

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Auth", .style = "sw-setting-label" });
    ca_select(&(Ca_SelectDesc){
        .options      = SSHW_AUTH_OPTIONS,
        .option_count = SSHW_AUTH_OPTION_COUNT,
        .selected     = w->auth_selected,
        .on_change    = sshw_on_auth_change,
        .change_data  = w,
        .style        = "sw-select",
    });
    ca_div_end();

    if (auth == SOL_SSH_AUTH_KEY) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
        ca_text(&(Ca_TextDesc){ .text = "Key file", .style = "sw-setting-label" });
        ca_input(&(Ca_InputDesc){
            .text = w->key_path, .placeholder = "~/.ssh/id_ed25519",
            .on_change = sshw_on_key_path_change, .change_data = w,
            .style = "search-input",
        });
        ca_btn_begin(&(Ca_BtnDesc){
            .text = "Browse", .style = "welcome-btn",
            .on_click = sshw_on_browse_click, .click_data = w,
        });
        ca_btn_end();
        ca_div_end();
    } else if (auth == SOL_SSH_AUTH_PASSWORD) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
        ca_text(&(Ca_TextDesc){ .text = "Password", .style = "sw-setting-label" });
        ca_input(&(Ca_InputDesc){
            .text = w->password, .placeholder = "prompted each connection",
            .input_mode = CA_INPUT_PASSWORD,
            .on_change = sshw_on_password_change, .change_data = w,
            .style = "search-input",
        });
        ca_div_end();
    } else {
        ca_text(&(Ca_TextDesc){
            .text = "Uses the running ssh-agent (SSH_AUTH_SOCK).",
            .style = "sw-setting-value",
        });
    }

    if (w->status_text[0] != '\0') {
        ca_text(&(Ca_TextDesc){ .text = w->status_text, .style = "sw-setting-value" });
    }

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_btn_begin(&(Ca_BtnDesc){
        .text = "Save", .style = "welcome-btn",
        .on_click = sshw_on_save_click, .click_data = w,
    });
    ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){
        .text = "Cancel", .style = "welcome-btn",
        .on_click = sshw_on_cancel_click, .click_data = w,
    });
    ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){
        .text = "Connect", .style = "welcome-btn-primary",
        .on_click = sshw_on_connect_click, .click_data = w,
    });
    ca_btn_end();
    ca_div_end();
}

static void sshw_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolSshConnectWindow *w = (SolSshConnectWindow *)user_data;
    (void)ca_signal_get_u32(w->sig_rev);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-left" });
    sshw_render_saved_list(w);
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-right" });
    sshw_render_form(w);
    ca_div_end();
}

/* ------------------------------------------------------------------ */
/* Window layout + lifecycle                                           */
/* ------------------------------------------------------------------ */

static void sshw_build_layout(SolSshConnectWindow *w)
{
    ca_ui_begin(w->window, &(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-root" });
    w->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "sw-body",
    });
    ca_div_set_builder(w->content_host, sshw_content_builder, w);
    ca_div_end();
    ca_ui_end();
}

static void sshw_destroy(SolSshConnectWindow *w)
{
    if (!w) return;
    if (w->window && ca_window_is_open(w->window))
        ca_window_close(w->window);
    free(w);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void sol_ui_ssh_window_open(Ca_Instance *instance,
                            SolSshConnectCallback on_connect,
                            void *user_data)
{
    if (!instance) return;
    if (g_sshw && g_sshw->window && ca_window_is_open(g_sshw->window)) return;

    SolSshConnectWindow *w = (SolSshConnectWindow *)calloc(1, sizeof(*w));
    if (!w) return;

    w->instance        = instance;
    w->on_connect      = on_connect;
    w->on_connect_data = user_data;
    snprintf(w->port_text, sizeof(w->port_text), "22");

    sol_ssh_config_load(&w->saved);
    if (w->saved.count > 0u) {
        w->password[0] = '\0';
        sshw_load_into_form(w, &w->saved.items[0]);
    }

    w->sig_rev = ca_signal_u32(instance, 0u);
    if (!w->sig_rev) { free(w); return; }

    w->window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = "Connect via SSH",
        .width  = SSHW_DEFAULT_WIDTH,
        .height = SSHW_DEFAULT_HEIGHT,
    });
    if (!w->window) { free(w); return; }

    sshw_build_layout(w);
    g_sshw = w;
}

void sol_ui_ssh_window_tick(void)
{
    if (!g_sshw) return;
    if (g_sshw->window && ca_window_is_open(g_sshw->window)) return;

    sshw_destroy(g_sshw);
    g_sshw = NULL;
    free(g_sshw_saved_ctxs);
    g_sshw_saved_ctxs = NULL;
    g_sshw_saved_ctx_count = 0u;
}
