// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_event.h — Sol's pub/sub event bus and event taxonomy.
 *
 * Two concerns live in this header because they form one contract:
 *
 *   1. The bus itself: SolEventBus, subscribe / unsubscribe, publish
 *      (synchronous) and post + drain (queued).
 *   2. The named events Sol publishes from its core: SOL_EVENT_*
 *      string constants and the payload struct that travels with
 *      each one.
 *
 * Stability rules for the taxonomy section:
 *   - Event names are stable dotted strings ("sol.<area>.<verb>").
 *   - Payload structs are versioned by name. New fields are appended
 *     to the end of the struct. Subscribers MUST check
 *     event->payload_size before reading any field beyond the
 *     original struct.
 *
 * Subscribing from a plugin's on_load(systems):
 *
 *     SolEventBus *bus = sol_system_events(systems);
 *     sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
 *         .event_name = SOL_EVENT_TEXT_EDITED,
 *         .handler    = on_text_edit,
 *         .user_data  = my_state,
 *     });
 */

#ifndef SOL_EVENT_H
#define SOL_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_buffer.h"   /* SolBufferId, SolBufferKind for payloads */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Bus primitives                                                      */
/* ================================================================== */

typedef uint64_t SolEventType;
typedef uint64_t SolSubscriptionToken;

typedef struct SolEventBus SolEventBus;

typedef struct SolEvent {
    SolEventType type;
    const char  *name;
    const void  *payload;
    size_t       payload_size;
    void        *sender;
    uint64_t     timestamp_ns;
} SolEvent;

typedef bool (*SolEventHandler)(const SolEvent *event, void *user_data);

enum {
    SOL_EVENT_FLAG_NONE            = 0u,
    SOL_EVENT_FLAG_STOP_ON_HANDLED = 1u << 0,
};

typedef struct SolEventDesc {
    SolEventType  event_type;
    const char   *event_name;
    const void   *payload;
    size_t        payload_size;
    void         *sender;
    uint32_t      flags;
} SolEventDesc;

typedef struct SolEventSubscriptionDesc {
    SolEventType    event_type;
    const char     *event_name;
    int             priority;
    SolEventHandler handler;
    void           *user_data;
} SolEventSubscriptionDesc;

typedef struct SolEventBusConfig {
    size_t initial_subscriber_capacity;
    size_t initial_queue_capacity;
} SolEventBusConfig;

SolEventBusConfig sol_event_bus_config_default(void);

SolEventBus *sol_event_bus_create(const SolEventBusConfig *config);
void         sol_event_bus_destroy(SolEventBus *bus);

SolEventType sol_event_type_from_name(const char *name);

SolSubscriptionToken sol_event_bus_subscribe(
    SolEventBus *bus,
    const SolEventSubscriptionDesc *desc);

bool   sol_event_bus_unsubscribe(SolEventBus *bus, SolSubscriptionToken token);

size_t sol_event_bus_publish(SolEventBus *bus, const SolEventDesc *desc);
bool   sol_event_bus_post   (SolEventBus *bus, const SolEventDesc *desc);
size_t sol_event_bus_drain  (SolEventBus *bus, size_t max_events);

/* ================================================================== */
/* Sol core event taxonomy                                             */
/* ================================================================== */

/* -- App lifecycle ------------------------------------------------- */
#define SOL_EVENT_APP_STARTUP        "sol.app.startup"
#define SOL_EVENT_APP_READY          "sol.app.ready"

/* -- Buffer lifecycle ---------------------------------------------- */
#define SOL_EVENT_BUFFER_OPENED      "sol.buffer.opened"
#define SOL_EVENT_BUFFER_CLOSED      "sol.buffer.closed"
#define SOL_EVENT_BUFFER_FOCUSED     "sol.buffer.focused"

/* -- Text editing -------------------------------------------------- */
#define SOL_EVENT_TEXT_EDITED        "sol.text.edited"

/* -- File tree ----------------------------------------------------- */
#define SOL_EVENT_FILE_TREE_ROOT     "sol.file_tree.root_changed"

/* -- Commands ------------------------------------------------------ */
#define SOL_EVENT_COMMAND_INVOKED    "sol.command.invoked"

/* SOL_EVENT_APP_STARTUP — fired exactly once, after subsystems are
   up but before the frame loop spins. */
typedef struct SolAppStartupPayload {
    uint32_t worker_count;
    uint32_t loaded_plugins;
    uint64_t warmup_checksum;
    bool     input_binding_active;
} SolAppStartupPayload;

/* SOL_EVENT_APP_READY — fired after the primary window exists and
   the first frame is about to run. No payload. */

/* SOL_EVENT_BUFFER_OPENED / CLOSED / FOCUSED.
 * `source_path` is the on-disk path for text buffers loaded from a
 * file; NULL for scratch/non-file buffers. All pointers are owned by
 * the buffer system and remain valid for the duration of the handler. */
typedef struct SolBufferEventPayload {
    SolBufferId   buffer_id;
    SolBufferKind kind;
    const char   *name;
    const char   *source_path;   /* may be NULL */
} SolBufferEventPayload;

/* SOL_EVENT_TEXT_EDITED — fired after a successful text-buffer
 * mutation. Models insert and remove as a single replace shape:
 *   pure insert : removed_bytes == 0, inserted_bytes  > 0
 *   pure remove : removed_bytes  > 0, inserted_bytes == 0
 * `byte_offset` is the rope offset BEFORE the change took effect. */
typedef struct SolTextEditedPayload {
    SolBufferId buffer_id;
    size_t      byte_offset;
    size_t      removed_bytes;
    size_t      inserted_bytes;
} SolTextEditedPayload;

/* SOL_EVENT_FILE_TREE_ROOT — fired when the tree mounts a new root.
   `path` is the new root (NULL when the tree was cleared). */
typedef struct SolFileTreeRootPayload {
    const char *path;
} SolFileTreeRootPayload;

/* SOL_EVENT_COMMAND_INVOKED — fired when a command-flow action runs
   (e.g. user typed the leader chord for "editor.save"). */
typedef struct SolCommandInvokedPayload {
    const char *action;
} SolCommandInvokedPayload;

/* ================================================================== */
/* Convenience publisher                                               */
/* ================================================================== */

/* Synchronous one-liner publish helper. No-op when `bus` is NULL,
   so call sites don't need to null-check before every publish. */
static inline void sol_event_publish(SolEventBus *bus,
                                     const char  *name,
                                     const void  *payload,
                                     size_t       payload_size,
                                     void        *sender)
{
    if (!bus || !name) return;
    SolEventDesc desc;
    desc.event_type   = 0u;
    desc.event_name   = name;
    desc.payload      = payload;
    desc.payload_size = payload_size;
    desc.sender       = sender;
    desc.flags        = SOL_EVENT_FLAG_NONE;
    (void)sol_event_bus_publish(bus, &desc);
}

#ifdef __cplusplus
}
#endif

#endif /* SOL_EVENT_H */
