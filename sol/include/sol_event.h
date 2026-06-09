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

/* A single event dispatched through the bus. Handlers receive a const pointer. */
typedef struct SolEvent {
    SolEventType type;
    const char  *name;
    const void  *payload;
    size_t       payload_size;
    void        *sender;
    uint64_t     timestamp_ns;
} SolEvent;

/*
 * Callback invoked for each matching event. Return true to mark the event
 * as handled (stops further dispatch when SOL_EVENT_FLAG_STOP_ON_HANDLED is set).
 */
typedef bool (*SolEventHandler)(const SolEvent *event, void *user_data);

enum {
    SOL_EVENT_FLAG_NONE            = 0u,
    SOL_EVENT_FLAG_STOP_ON_HANDLED = 1u << 0,
};

/* Descriptor used to publish a single event. Fill event_name OR event_type. */
typedef struct SolEventDesc {
    SolEventType  event_type;
    const char   *event_name;
    const void   *payload;
    size_t        payload_size;
    void         *sender;
    uint32_t      flags;
} SolEventDesc;

/* Descriptor used to register a subscription. Fill event_name OR event_type. */
typedef struct SolEventSubscriptionDesc {
    SolEventType    event_type;
    const char     *event_name;
    int             priority;
    SolEventHandler handler;
    void           *user_data;
} SolEventSubscriptionDesc;

/* Tuning knobs for the event bus's internal storage. */
typedef struct SolEventBusConfig {
    size_t initial_subscriber_capacity;
    size_t initial_queue_capacity;
} SolEventBusConfig;

/* Return a SolEventBusConfig populated with sensible defaults. */
SolEventBusConfig sol_event_bus_config_default(void);

/*
 * Create a new event bus.
 *
 * config  Capacity hints; use sol_event_bus_config_default() for defaults.
 * Returns A heap-allocated bus, or NULL on allocation failure.
 */
SolEventBus *sol_event_bus_create(const SolEventBusConfig *config);

/* Destroy the event bus and free all internal resources. */
void         sol_event_bus_destroy(SolEventBus *bus);

/*
 * Derive a stable numeric type id from an event name string.
 *
 * name    Dotted event name, e.g. "sol.text.edited".
 * Returns A non-zero numeric type suitable for use in SolEventDesc.event_type.
 */
SolEventType sol_event_type_from_name(const char *name);

/*
 * Subscribe to an event by name or type.
 *
 * bus   The event bus.
 * desc  Subscription parameters (event name/type, handler, priority).
 * Returns  An opaque token used to unsubscribe later.
 */
SolSubscriptionToken sol_event_bus_subscribe(
    SolEventBus *bus,
    const SolEventSubscriptionDesc *desc);

/*
 * Unsubscribe a previously registered handler.
 *
 * bus    The event bus.
 * token  Token returned by sol_event_bus_subscribe.
 * Returns  true if the subscription was found and removed.
 */
bool   sol_event_bus_unsubscribe(SolEventBus *bus, SolSubscriptionToken token);

/*
 * Synchronously dispatch an event to all matching subscribers.
 *
 * bus   The event bus.
 * desc  Event to publish.
 * Returns  Number of handlers that returned true (handled).
 */
size_t sol_event_bus_publish(SolEventBus *bus, const SolEventDesc *desc);

/*
 * Enqueue an event for deferred delivery via sol_event_bus_drain.
 *
 * bus   The event bus.
 * desc  Event to enqueue.
 * Returns  true on success, false if the queue is full.
 */
bool   sol_event_bus_post   (SolEventBus *bus, const SolEventDesc *desc);

/*
 * Dispatch up to max_events queued events synchronously.
 *
 * bus        The event bus.
 * max_events Maximum number of events to dequeue and deliver.
 * Returns    Number of events actually dispatched.
 */
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

/*
 * Payload for SOL_EVENT_APP_STARTUP — fired exactly once after subsystems
 * are up but before the frame loop spins.
 */
typedef struct SolAppStartupPayload {
    uint32_t worker_count;
    uint32_t loaded_plugins;
    uint64_t warmup_checksum;
    bool     input_binding_active;
} SolAppStartupPayload;

/* SOL_EVENT_APP_READY — fired after the primary window exists and
   the first frame is about to run. No payload. */

/*
 * Payload for SOL_EVENT_BUFFER_OPENED, CLOSED, and FOCUSED.
 *
 * source_path is the on-disk path for file-backed text buffers, or NULL for
 * scratch/non-file buffers. All pointers are owned by the buffer system and
 * remain valid for the duration of the handler.
 */
typedef struct SolBufferEventPayload {
    SolBufferId   buffer_id;
    SolBufferKind kind;
    const char   *name;
    const char   *source_path;   /* may be NULL */
} SolBufferEventPayload;

/*
 * Payload for SOL_EVENT_TEXT_EDITED — fired after a successful text-buffer
 * mutation. Inserts and removals are represented as a single replace shape:
 *   pure insert:  removed_bytes == 0, inserted_bytes > 0
 *   pure remove:  removed_bytes > 0,  inserted_bytes == 0
 * byte_offset is the rope offset BEFORE the change took effect.
 */
typedef struct SolTextEditedPayload {
    SolBufferId buffer_id;
    size_t      byte_offset;
    size_t      removed_bytes;
    size_t      inserted_bytes;
} SolTextEditedPayload;

/*
 * Payload for SOL_EVENT_FILE_TREE_ROOT — fired when the tree mounts a new root.
 * path is the new root directory, or NULL when the tree was cleared.
 */
typedef struct SolFileTreeRootPayload {
    const char *path;
} SolFileTreeRootPayload;

/*
 * Payload for SOL_EVENT_COMMAND_INVOKED — fired when a command-flow action
 * runs (e.g. the user typed the leader chord for "editor.save").
 */
typedef struct SolCommandInvokedPayload {
    const char *action;
} SolCommandInvokedPayload;

/* ================================================================== */
/* Convenience publisher                                               */
/* ================================================================== */

/*
 * Synchronously publish an event by name, with a null-check on the bus.
 *
 * bus          The event bus, or NULL (no-op when NULL).
 * name         Dotted event name string.
 * payload      Pointer to the event payload struct.
 * payload_size Size in bytes of the payload struct.
 * sender       Opaque sender pointer passed through to handlers.
 */
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
