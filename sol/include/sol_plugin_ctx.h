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

#ifndef SOL_API
#if defined(_WIN32) && defined(sol_EXPORTS)
#define SOL_API __declspec(dllexport)
#else
#define SOL_API
#endif
#endif

/* Compiler annotations.
 * MSVC's cl.exe does not understand GCC-style __attribute__ syntax, but
 * GCC/Clang can still use it to validate printf-style calls at compile time.
 */
#ifndef SOL_PRINTF_FORMAT
#if defined(__has_attribute)
#if __has_attribute(format)
#define SOL_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define SOL_PRINTF_FORMAT(format_index, first_arg)
#endif
#elif defined(__GNUC__)
#define SOL_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define SOL_PRINTF_FORMAT(format_index, first_arg)
#endif
#endif

/* Forward-declare so plugins don't need causality.h in their include path. */
typedef struct SolUISystem SolUISystem;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolPluginCtx    SolPluginCtx;
typedef struct SolSystemManager SolSystemManager;
typedef void (*SolServiceDestroyFn)(void *service, void *user_data);
typedef uint32_t SolPluginSidePanelToken;
#define SOL_PLUGIN_SIDE_PANEL_TOKEN_INVALID 0u

/* Render callback for a plugin-owned workspace side panel. */
typedef void (*SolPluginSidePanelRenderFn)(void *user_data);

/* UI-thread tick callback for adopting completed asynchronous work. */
typedef void (*SolPluginSidePanelTickFn)(void *user_data);

/* Descriptor for a plugin-owned workspace side panel. */
typedef struct SolPluginSidePanelDesc {
    const char                   *id;
    const char                   *title;
    SolPluginSidePanelRenderFn    render;
    SolPluginSidePanelTickFn      tick;
    void                         *user_data;
} SolPluginSidePanelDesc;

/* ================================================================== */
/* Core subsystem access                                               */
/* ================================================================== */

/* Returns the system manager associated with this plugin context. */
SOL_API SolSystemManager *sol_plugin_systems(SolPluginCtx *ctx);

/* Returns the event bus associated with this plugin context. */
SOL_API SolEventBus      *sol_plugin_event_bus(SolPluginCtx *ctx);

/* Returns the buffer system associated with this plugin context. */
SOL_API SolBufferSystem  *sol_plugin_buffers(SolPluginCtx *ctx);

/* Returns the job system associated with this plugin context. */
SOL_API SolJobSystem     *sol_plugin_jobs(SolPluginCtx *ctx);

/* Returns the input system associated with this plugin context. */
SOL_API SolInputSystem   *sol_plugin_input(SolPluginCtx *ctx);

/*
 * Returns the UI system, or NULL if it was not registered before plugins loaded.
 *
 * Register it with: sol_system_register_service(systems, "sol.ui", ui, NULL, NULL).
 */
SOL_API SolUISystem      *sol_plugin_ui(SolPluginCtx *ctx);

/* ================================================================== */
/* Plugin metadata                                                     */
/* ================================================================== */

/* Returns the unique dotted id of this plugin (e.g. "com.myco.myplugin"). */
SOL_API const char *sol_plugin_id(const SolPluginCtx *ctx);

/* Returns the human-readable display name of this plugin. */
SOL_API const char *sol_plugin_display_name(const SolPluginCtx *ctx);

/* Returns the semver version string of this plugin (e.g. "1.0.0"). */
SOL_API const char *sol_plugin_version(const SolPluginCtx *ctx);

/*
 * Thread-safe printf-style logger prefixed with "[plugin:<id>] ".
 *
 * ctx  The plugin context.
 * fmt  printf format string. No trailing newline needed.
 */
SOL_API void sol_plugin_log(SolPluginCtx *ctx, const char *fmt, ...)
    SOL_PRINTF_FORMAT(2, 3);

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

SOL_API bool sol_plugin_register_language(SolPluginCtx      *ctx,
                                           const void        *language,
                                           const char *const *extensions);

/* Variant that also supplies the raw highlights.scm query text so Sol
 * can use the full TSQuery pipeline for syntax highlighting.  Pass
 * query_scm=NULL to fall back to the built-in heuristic walker. */
SOL_API bool sol_plugin_register_language_with_query(
                                   SolPluginCtx      *ctx,
                                   const void        *language,
                                   const char *const *extensions,
                                   const char        *query_scm);

/* ================================================================== */
/* Event subscriptions                                                 */
/*                                                                     */
/* All subscriptions registered through the plugin context are         */
/* automatically unsubscribed when the plugin is unloaded.             */
/* ================================================================== */

/*
 * Subscribe to a named event; auto-unsubscribed when the plugin unloads.
 *
 * ctx         The plugin context.
 * event_name  Dotted event name, e.g. SOL_EVENT_TEXT_EDITED.
 * handler     Function invoked for each matching event.
 * user_data   Passed unchanged to handler.
 * Returns     An opaque token used to unsubscribe early.
 */
SOL_API SolSubscriptionToken sol_plugin_subscribe(
    SolPluginCtx    *ctx,
    const char      *event_name,
    SolEventHandler  handler,
    void            *user_data);

/*
 * Unsubscribe a previously registered event handler before plugin unload.
 *
 * ctx    The plugin context.
 * token  Token returned by sol_plugin_subscribe.
 */
SOL_API void sol_plugin_unsubscribe(SolPluginCtx *ctx, SolSubscriptionToken token);

/* ================================================================== */
/* Command registration                                                */
/*                                                                     */
/* Commands appear in the command palette and can be bound to key      */
/* chords. The action string must be dot-namespaced and unique across  */
/* all plugins, e.g. "com.myco.myplugin.format".                       */
/*                                                                     */
/* Registered commands are automatically unregistered on unload.       */
/* ================================================================== */

/* Descriptor for a plugin command registered via sol_plugin_register_command. */
typedef struct SolPluginCommandDesc {
    const char            *action;       /* unique dot-namespaced id          */
    const char            *label;        /* human-readable, shown in palette  */
    const SolKeyCode      *chord;        /* key sequence (NULL = palette-only) */
    const SolModifierMask *chord_mods;   /* per-step modifiers (NULL = none)  */
    size_t                 chord_length; /* number of steps in chord          */
    SolInputActionCallback callback;     /* invoked when command fires        */
    void                  *user_data;
} SolPluginCommandDesc;

/*
 * Register a command; auto-unregistered when the plugin unloads.
 *
 * ctx   The plugin context.
 * desc  Command parameters including action id, label, optional chord, and callback.
 * Returns  true on success.
 */
SOL_API bool sol_plugin_register_command(SolPluginCtx *ctx, const SolPluginCommandDesc *desc);

/*
 * Unregister a command before plugin unload.
 *
 * ctx     The plugin context.
 * action  The action id string used when the command was registered.
 */
SOL_API void sol_plugin_unregister_command(SolPluginCtx *ctx, const char *action);

/* ================================================================== */
/* Key binding registration                                            */
/*                                                                     */
/* Binds a key directly (outside the leader-chord mechanism).          */
/* Auto-unbound on plugin unload.                                      */
/* ================================================================== */

/*
 * Bind a key directly (outside the leader-chord mechanism); auto-unbound on unload.
 *
 * ctx   The plugin context.
 * desc  Binding parameters.
 * Returns  An opaque token used to unbind early.
 */
SOL_API SolInputActionToken sol_plugin_bind_key(SolPluginCtx *ctx,
                                                 const SolInputBindingDesc *desc);

/*
 * Unbind a key binding before plugin unload.
 *
 * ctx    The plugin context.
 * token  Token returned by sol_plugin_bind_key.
 */
SOL_API void sol_plugin_unbind_key(SolPluginCtx *ctx, SolInputActionToken token);

/* ================================================================== */
/* Status bar segments                                                 */
/*                                                                     */
/* Plugins can contribute text segments to the right side of the       */
/* status bar. Segments are shown in registration order.               */
/* Tokens are auto-removed on plugin unload.                           */
/* ================================================================== */

typedef uint32_t SolPluginStatusToken;
#define SOL_PLUGIN_STATUS_TOKEN_INVALID 0u

/*
 * Add a text segment to the right side of the status bar.
 *
 * ctx          The plugin context.
 * text         Text to display.
 * style_class  CSS class for styling, or NULL for the default style.
 * Returns      An opaque token used to update or remove the segment later;
 *              SOL_PLUGIN_STATUS_TOKEN_INVALID on failure.
 */
SOL_API SolPluginStatusToken sol_plugin_add_status_segment(
    SolPluginCtx *ctx,
    const char   *text,
    const char   *style_class);

/*
 * Update the text of an existing status bar segment.
 *
 * ctx    The plugin context.
 * token  Token returned by sol_plugin_add_status_segment.
 * text   New text to display.
 */
SOL_API void sol_plugin_update_status_segment(SolPluginCtx        *ctx,
                                               SolPluginStatusToken token,
                                               const char          *text);

/*
 * Remove a status bar segment.
 *
 * ctx    The plugin context.
 * token  Token returned by sol_plugin_add_status_segment.
 */
SOL_API void sol_plugin_remove_status_segment(SolPluginCtx        *ctx,
                                               SolPluginStatusToken token);

/* Register a native workspace side panel; automatically removed on unload. */
SOL_API SolPluginSidePanelToken sol_plugin_register_side_panel(
    SolPluginCtx *ctx,
    const SolPluginSidePanelDesc *desc);

/* Remove a side panel before plugin unload. */
SOL_API void sol_plugin_unregister_side_panel(SolPluginCtx *ctx,
                                               SolPluginSidePanelToken token);

/* Show or hide a registered side panel. */
SOL_API bool sol_plugin_show_side_panel(SolPluginCtx *ctx,
                                        SolPluginSidePanelToken token);
SOL_API void sol_plugin_hide_side_panel(SolPluginCtx *ctx,
                                        SolPluginSidePanelToken token);

/* Return whether a registered side panel is currently visible. */
SOL_API bool sol_plugin_side_panel_visible(SolPluginCtx *ctx,
                                           SolPluginSidePanelToken token);

/* Rebuild a side panel after its UI-thread state changes. */
SOL_API void sol_plugin_notify_side_panel(SolPluginCtx *ctx,
                                          SolPluginSidePanelToken token);

/* Wake the editor event loop after a worker publishes completion state. */
SOL_API void sol_plugin_wake_ui(SolPluginCtx *ctx);

/* ================================================================== */
/* Buffer operations                                                   */
/* ================================================================== */

/*
 * Open a file-backed text buffer, deduplicating by source path.
 *
 * ctx   The plugin context.
 * path  Absolute path of the file to open.
 * Returns  Buffer id (may refer to an existing buffer), or 0 on error.
 */
SOL_API SolBufferId sol_plugin_open_file(SolPluginCtx *ctx, const char *path);

/*
 * Open an in-memory scratch text buffer.
 *
 * ctx           The plugin context.
 * name          Display name shown in tab strips.
 * initial_text  Initial content, or NULL for an empty buffer.
 * initial_len   Byte length of initial_text.
 * source_path   Optional path used for open-by-path deduplication; may be NULL.
 * Returns       Buffer id, or 0 on error.
 */
SOL_API SolBufferId sol_plugin_open_scratch(SolPluginCtx *ctx,
                                             const char   *name,
                                             const char   *initial_text,
                                             size_t        initial_len,
                                             const char   *source_path);

/*
 * Open a fully custom buffer with caller-provided render and destroy callbacks.
 *
 * ctx    The plugin context.
 * name   Display name for the buffer.
 * state  Caller-owned state pointer passed to ops callbacks.
 * ops    Vtable of destroy and render callbacks.
 * Returns  Buffer id, or 0 on error.
 */
SOL_API SolBufferId sol_plugin_open_custom(SolPluginCtx  *ctx,
                                            const char    *name,
                                            void          *state,
                                            SolBufferOps   ops);

/*
 * Focus a buffer in the active leaf.
 *
 * ctx  The plugin context.
 * id   Id of the buffer to focus.
 * Returns  true on success.
 */
SOL_API bool sol_plugin_focus_buffer(SolPluginCtx *ctx, SolBufferId id);

/* Returns the id of the currently-focused buffer, or 0 if none. */
SOL_API SolBufferId sol_plugin_active_buffer(SolPluginCtx *ctx);

/*
 * Insert bytes into a text buffer at a byte offset.
 *
 * ctx          The plugin context.
 * id           Buffer to edit (must be a text buffer).
 * byte_offset  Insertion point in the rope.
 * text         Bytes to insert.
 * len          Number of bytes to insert.
 * Returns      false for non-text buffers or on failure.
 */
SOL_API bool   sol_plugin_buf_insert(SolPluginCtx *ctx, SolBufferId id,
                                      size_t byte_offset,
                                      const char *text, size_t len);

/*
 * Delete bytes from a text buffer at a byte offset.
 *
 * ctx          The plugin context.
 * id           Buffer to edit (must be a text buffer).
 * byte_offset  Start of the region to delete.
 * byte_count   Number of bytes to remove.
 * Returns      false for non-text buffers or on failure.
 */
SOL_API bool   sol_plugin_buf_delete(SolPluginCtx *ctx, SolBufferId id,
                                      size_t byte_offset, size_t byte_count);

/*
 * Copy up to out_size-1 bytes from a text buffer into out, NUL-terminating.
 *
 * ctx          The plugin context.
 * id           Buffer to read from.
 * byte_offset  Starting byte offset in the rope.
 * out          Destination buffer.
 * out_size     Total bytes available in out (including space for the NUL).
 * Returns      Number of bytes copied, excluding the NUL terminator.
 */
SOL_API size_t sol_plugin_buf_read(SolPluginCtx *ctx, SolBufferId id,
                                    size_t byte_offset,
                                    char *out, size_t out_size);

/*
 * Return the total byte length of a text buffer's content.
 *
 * Returns 0 for non-text buffers.
 */
SOL_API size_t sol_plugin_buf_length(SolPluginCtx *ctx, SolBufferId id);

/* Returns the cursor byte offset in a text buffer (0 for non-text). */
SOL_API size_t sol_plugin_buf_cursor(SolPluginCtx *ctx, SolBufferId id);

/*
 * Move the cursor in a text buffer to a specific byte offset.
 *
 * ctx          The plugin context.
 * id           Buffer to update (must be a text buffer).
 * byte_offset  Target byte offset; clamped to rope length.
 * Returns      false for non-text buffers.
 */
SOL_API bool   sol_plugin_buf_set_cursor(SolPluginCtx *ctx, SolBufferId id,
                                          size_t byte_offset);

/* ================================================================== */
/* Async job submission                                                */
/* ================================================================== */

/*
 * Submit a job to the shared thread pool.
 *
 * ctx        The plugin context.
 * fn         Function to execute on a worker thread.
 * user_data  Passed unchanged to fn.
 * fence      Optional fence to track completion; pass NULL to ignore.
 * Returns    true if the job was enqueued successfully.
 */
SOL_API bool sol_plugin_submit_job(SolPluginCtx *ctx,
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

/*
 * Register a versioned service; auto-unregistered on plugin unload.
 *
 * ctx                The plugin context.
 * name               Unique service name.
 * version            Service ABI version number.
 * service            Pointer to the service object.
 * destroy_fn         Optional destructor called on unregister; may be NULL.
 * destroy_user_data  Passed unchanged to destroy_fn.
 * Returns            true on success.
 */
SOL_API bool sol_plugin_register_service(SolPluginCtx       *ctx,
                                           const char         *name,
                                           uint32_t            version,
                                           void               *service,
                                           SolServiceDestroyFn destroy_fn,
                                           void               *destroy_user_data);

/*
 * Retrieve a registered service, enforcing a minimum version.
 *
 * ctx          The plugin context.
 * name         Service name to look up.
 * min_version  Minimum acceptable version; returns NULL if lower.
 * Returns      The service pointer, or NULL if not found or version too low.
 */
SOL_API void *sol_plugin_get_service(SolPluginCtx *ctx,
                                       const char   *name,
                                       uint32_t      min_version);

#ifdef __cplusplus
}
#endif

#endif /* SOL_PLUGIN_CTX_H */
