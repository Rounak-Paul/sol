// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_plugin_ctx.h — The complete plugin API surface.
 *
 * Every plugin receives a SolPluginCtx* through its on_load() callback.
 * This object is valid for the entire plugin lifetime and provides
 * access to all editor capabilities:
 *
 *   - Subsystem access (buffers, jobs, events, input, UI)
 *   - Event subscriptions     — auto-cleaned on unload
 *   - Command registration    — auto-cleaned on unload
 *   - Key binding registration — auto-cleaned on unload
 *   - Status bar segments     — auto-cleaned on unload
 *   - Buffer operations (open, focus, read, write, cursor)
 *   - Async job submission
 *   - Versioned service registry
 *   - Logging
 *
 * IMPORTANT: Do not cache raw pointers returned by sol_plugin_*
 * accessors beyond the call frame. Use the ctx accessors every time;
 * they are cheap and null-safe.
 */

#ifndef SOL_PLUGIN_CTX_H
#define SOL_PLUGIN_CTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_buffer.h"
#include "sol_event.h"
#include "sol_input.h"
#include "sol_job.h"

/* Forward-declare so plugins don't need causality.h in their include path. */
typedef struct SolUISystem SolUISystem;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolPluginCtx    SolPluginCtx;
typedef struct SolSystemManager SolSystemManager;
typedef void (*SolServiceDestroyFn)(void *service, void *user_data);

/* ================================================================== */
/* Core subsystem access                                               */
/* ================================================================== */

SolSystemManager *sol_plugin_systems(SolPluginCtx *ctx);
SolEventBus      *sol_plugin_event_bus(SolPluginCtx *ctx);
SolBufferSystem  *sol_plugin_buffers(SolPluginCtx *ctx);
SolJobSystem     *sol_plugin_jobs(SolPluginCtx *ctx);
SolInputSystem   *sol_plugin_input(SolPluginCtx *ctx);

/* Returns the UI system, or NULL if it was not registered before
 * plugins were loaded. Register it with:
 *   sol_system_register_service(systems, "sol.ui", ui, NULL, NULL); */
SolUISystem      *sol_plugin_ui(SolPluginCtx *ctx);

/* ================================================================== */
/* Plugin metadata                                                     */
/* ================================================================== */

const char *sol_plugin_id(const SolPluginCtx *ctx);
const char *sol_plugin_display_name(const SolPluginCtx *ctx);
const char *sol_plugin_version(const SolPluginCtx *ctx);

/* Thread-safe printf-style logger.  Output is prefixed with
 * "[plugin:<id>] " for easy grep.  No trailing newline needed. */
void sol_plugin_log(SolPluginCtx *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ================================================================== */
/* Syntax / Language registration                                      */
/*                                                                     */
/* Register a tree-sitter language with the syntax registry so Sol    */
/* can use it for syntax highlighting.                                 */
/*                                                                     */
/* `language`   — const TSLanguage* cast to const void* (keeps this   */
/*                header free of the tree-sitter dependency).          */
/* `extensions` — NULL-terminated array, e.g. { ".c", ".h", NULL }.  */
/*                                                                     */
/* Returns false when the syntax registry was not attached or the     */
/* registry is full.                                                   */
/* ================================================================== */

bool sol_plugin_register_language(SolPluginCtx      *ctx,
                                   const void        *language,
                                   const char *const *extensions);

/* ================================================================== */
/* Event subscriptions                                                 */
/*                                                                     */
/* All subscriptions registered through the plugin context are         */
/* automatically unsubscribed when the plugin is unloaded.             */
/* ================================================================== */

SolSubscriptionToken sol_plugin_subscribe(
    SolPluginCtx    *ctx,
    const char      *event_name,
    SolEventHandler  handler,
    void            *user_data);

void sol_plugin_unsubscribe(SolPluginCtx *ctx, SolSubscriptionToken token);

/* ================================================================== */
/* Command registration                                                */
/*                                                                     */
/* Commands appear in the command palette and can be bound to key      */
/* chords. The action string must be dot-namespaced and unique across  */
/* all plugins, e.g. "com.myco.myplugin.format".                       */
/*                                                                     */
/* Registered commands are automatically unregistered on unload.       */
/* ================================================================== */

typedef struct SolPluginCommandDesc {
    const char            *action;       /* unique dot-namespaced id          */
    const char            *label;        /* human-readable, shown in palette  */
    const SolKeyCode      *chord;        /* key sequence (NULL = palette-only) */
    const SolModifierMask *chord_mods;   /* per-step modifiers (NULL = none)  */
    size_t                 chord_length; /* number of steps in chord          */
    SolInputActionCallback callback;     /* invoked when command fires        */
    void                  *user_data;
} SolPluginCommandDesc;

bool sol_plugin_register_command(SolPluginCtx *ctx, const SolPluginCommandDesc *desc);
void sol_plugin_unregister_command(SolPluginCtx *ctx, const char *action);

/* ================================================================== */
/* Key binding registration                                            */
/*                                                                     */
/* Binds a key directly (outside the leader-chord mechanism).          */
/* Auto-unbound on plugin unload.                                      */
/* ================================================================== */

SolInputActionToken sol_plugin_bind_key(SolPluginCtx *ctx,
                                         const SolInputBindingDesc *desc);
void sol_plugin_unbind_key(SolPluginCtx *ctx, SolInputActionToken token);

/* ================================================================== */
/* Status bar segments                                                 */
/*                                                                     */
/* Plugins can contribute text segments to the right side of the       */
/* status bar. Segments are shown in registration order.               */
/* Tokens are auto-removed on plugin unload.                           */
/* ================================================================== */

typedef uint32_t SolPluginStatusToken;
#define SOL_PLUGIN_STATUS_TOKEN_INVALID 0u

SolPluginStatusToken sol_plugin_add_status_segment(
    SolPluginCtx *ctx,
    const char   *text,
    const char   *style_class);   /* CSS class, or NULL for default */

void sol_plugin_update_status_segment(SolPluginCtx        *ctx,
                                       SolPluginStatusToken token,
                                       const char          *text);

void sol_plugin_remove_status_segment(SolPluginCtx        *ctx,
                                       SolPluginStatusToken token);

/* ================================================================== */
/* Buffer operations                                                   */
/* ================================================================== */

/* Open a file into a text buffer, deduplicating by source path.
 * Returns the buffer id (may be an existing buffer), or 0 on error. */
SolBufferId sol_plugin_open_file(SolPluginCtx *ctx, const char *path);

/* Open a scratch (in-memory) text buffer.  `initial_text` may be NULL.
 * `source_path` is optional and used only for dedup (may be NULL). */
SolBufferId sol_plugin_open_scratch(SolPluginCtx *ctx,
                                     const char   *name,
                                     const char   *initial_text,
                                     size_t        initial_len,
                                     const char   *source_path);

/* Open a fully custom plugin buffer with caller-provided ops. */
SolBufferId sol_plugin_open_custom(SolPluginCtx  *ctx,
                                    const char    *name,
                                    void          *state,
                                    SolBufferOps   ops);

/* Focus a buffer in the active leaf. */
bool sol_plugin_focus_buffer(SolPluginCtx *ctx, SolBufferId id);

/* Return the currently-focused buffer id (0 if none). */
SolBufferId sol_plugin_active_buffer(SolPluginCtx *ctx);

/* Text buffer editing — byte-offset based.
 * All three functions return false for non-text buffers. */
bool   sol_plugin_buf_insert(SolPluginCtx *ctx, SolBufferId id,
                              size_t byte_offset,
                              const char *text, size_t len);
bool   sol_plugin_buf_delete(SolPluginCtx *ctx, SolBufferId id,
                              size_t byte_offset, size_t byte_count);

/* Copy up to out_size-1 bytes from the buffer into `out`, NUL-terminating.
 * Returns the number of bytes actually copied (excluding the NUL). */
size_t sol_plugin_buf_read(SolPluginCtx *ctx, SolBufferId id,
                            size_t byte_offset,
                            char *out, size_t out_size);

/* Total byte length of the buffer content (0 for non-text). */
size_t sol_plugin_buf_length(SolPluginCtx *ctx, SolBufferId id);

/* Cursor byte offset in a text buffer. */
size_t sol_plugin_buf_cursor(SolPluginCtx *ctx, SolBufferId id);
bool   sol_plugin_buf_set_cursor(SolPluginCtx *ctx, SolBufferId id,
                                  size_t byte_offset);

/* ================================================================== */
/* Async job submission                                                */
/* ================================================================== */

bool sol_plugin_submit_job(SolPluginCtx *ctx,
                            SolJobFn      fn,
                            void         *user_data,
                            SolJobFence  *fence);

/* ================================================================== */
/* Versioned service registry                                          */
/*                                                                     */
/* Plugin-registered services carry an explicit version number.        */
/* sol_plugin_get_service returns NULL when the registered version is  */
/* lower than `min_version`, protecting callers from stale ABI.        */
/*                                                                     */
/* Services registered here are automatically unregistered on unload   */
/* if a destroy_fn was provided.                                        */
/* ================================================================== */

bool sol_plugin_register_service(SolPluginCtx       *ctx,
                                   const char         *name,
                                   uint32_t            version,
                                   void               *service,
                                   SolServiceDestroyFn destroy_fn,
                                   void               *destroy_user_data);

/* Returns the service pointer, or NULL if not found or version too low. */
void *sol_plugin_get_service(SolPluginCtx *ctx,
                               const char   *name,
                               uint32_t      min_version);

#ifdef __cplusplus
}
#endif

#endif /* SOL_PLUGIN_CTX_H */
