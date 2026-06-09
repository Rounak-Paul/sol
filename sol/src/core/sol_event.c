#include "sol_event.h"

#include "sol_platform.h"
#include "sol_threading.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SolSubscription {
    SolSubscriptionToken token;
    SolEventType event_type;
    int priority;
    SolEventHandler handler;
    void *user_data;
    bool active;
} SolSubscription;

typedef struct SolQueuedEvent {
    SolEvent event;
    uint32_t flags;
    void *payload_copy;
    char *name_copy;
} SolQueuedEvent;

struct SolEventBus {
    pthread_mutex_t lock;

    SolSubscription *subscriptions;
    size_t subscription_count;
    size_t subscription_capacity;
    SolSubscriptionToken next_token;

    SolQueuedEvent *queue;
    size_t queue_capacity;
    size_t queue_count;
    size_t queue_head;
    size_t queue_tail;
};

/* Return the current monotonic time in nanoseconds. */
static uint64_t sol_now_ns(void)
{
    return sol_platform_now_monotonic_ns();
}

/*
 * Duplicate a string into a heap-allocated buffer.
 *
 * value   Source string; NULL is accepted and returns NULL.
 * Returns Heap-allocated copy, or NULL on OOM or NULL input.
 */
static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *out = (char *)malloc(len + 1u);
    if (!out) {
        return NULL;
    }

    memcpy(out, value, len + 1u);
    return out;
}

/*
 * Return a numeric event type: use explicit_type when non-zero, otherwise
 * hash the name string to derive a type id.
 *
 * explicit_type  Pre-computed type id, or 0 to force name-based lookup.
 * name           Event name string; ignored when explicit_type is non-zero.
 * Returns        Resolved SolEventType.
 */
static SolEventType sol_resolve_type(SolEventType explicit_type, const char *name)
{
    if (explicit_type != 0u) {
        return explicit_type;
    }

    return sol_event_type_from_name(name);
}

/*
 * Grow the subscription array to hold at least min_capacity entries.
 *
 * bus           Event bus whose subscription array is grown.
 * min_capacity  Minimum required capacity.
 * Returns       true on success or if capacity is already sufficient.
 */
static bool sol_event_bus_reserve_subscriptions(SolEventBus *bus, size_t min_capacity)
{
    if (bus->subscription_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = bus->subscription_capacity == 0u ? 16u : bus->subscription_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolSubscription *new_items = (SolSubscription *)realloc(
        bus->subscriptions,
        new_capacity * sizeof(SolSubscription)
    );

    if (!new_items) {
        return false;
    }

    bus->subscriptions = new_items;
    bus->subscription_capacity = new_capacity;
    return true;
}

/*
 * Grow the event queue to hold at least min_capacity entries.
 *
 * Copies the existing ring-buffer contents into the larger allocation so the
 * logical order is preserved.
 *
 * bus           Event bus whose queue is grown.
 * min_capacity  Minimum required capacity.
 * Returns       true on success or if capacity is already sufficient.
 */
static bool sol_event_bus_reserve_queue(SolEventBus *bus, size_t min_capacity)
{
    if (bus->queue_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = bus->queue_capacity == 0u ? 32u : bus->queue_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolQueuedEvent *new_queue = (SolQueuedEvent *)calloc(new_capacity, sizeof(SolQueuedEvent));
    if (!new_queue) {
        return false;
    }

    for (size_t i = 0; i < bus->queue_count; ++i) {
        const size_t source_index = (bus->queue_head + i) % bus->queue_capacity;
        new_queue[i] = bus->queue[source_index];
    }

    free(bus->queue);
    bus->queue = new_queue;
    bus->queue_capacity = new_capacity;
    bus->queue_head = 0u;
    bus->queue_tail = bus->queue_count;
    return true;
}

/*
 * Remove inactive subscriptions from the array (must be called under lock).
 *
 * bus  Event bus to compact.
 */
static void sol_event_bus_compact_locked(SolEventBus *bus)
{
    size_t write = 0u;
    for (size_t read = 0; read < bus->subscription_count; ++read) {
        if (!bus->subscriptions[read].active) {
            continue;
        }

        if (write != read) {
            bus->subscriptions[write] = bus->subscriptions[read];
        }
        ++write;
    }

    bus->subscription_count = write;
}

/*
 * Dispatch an event synchronously to all matching subscribers.
 *
 * Takes a snapshot of the subscriber list under lock, then calls each handler
 * outside the lock. Stops early if a handler returns true and
 * SOL_EVENT_FLAG_STOP_ON_HANDLED is set.
 *
 * bus     Event bus to dispatch through.
 * event   Event to deliver.
 * flags   Dispatch flags (e.g. SOL_EVENT_FLAG_STOP_ON_HANDLED).
 * Returns Number of handlers invoked.
 */
static size_t sol_event_bus_dispatch(SolEventBus *bus, const SolEvent *event, uint32_t flags)
{
    SolSubscription *snapshot = NULL;
    size_t snapshot_count = 0u;

    pthread_mutex_lock(&bus->lock);
    for (size_t i = 0; i < bus->subscription_count; ++i) {
        const SolSubscription *subscription = &bus->subscriptions[i];
        if (!subscription->active) {
            continue;
        }
        if (subscription->event_type != 0u && subscription->event_type != event->type) {
            continue;
        }
        ++snapshot_count;
    }

    if (snapshot_count > 0u) {
        snapshot = (SolSubscription *)malloc(snapshot_count * sizeof(SolSubscription));
        if (!snapshot) {
            pthread_mutex_unlock(&bus->lock);
            return 0u;
        }

        size_t cursor = 0u;
        for (size_t i = 0; i < bus->subscription_count; ++i) {
            const SolSubscription *subscription = &bus->subscriptions[i];
            if (!subscription->active) {
                continue;
            }
            if (subscription->event_type != 0u && subscription->event_type != event->type) {
                continue;
            }
            snapshot[cursor++] = *subscription;
        }
    }
    pthread_mutex_unlock(&bus->lock);

    size_t dispatched = 0u;
    for (size_t i = 0; i < snapshot_count; ++i) {
        if (!snapshot[i].handler) {
            continue;
        }

        const bool handled = snapshot[i].handler(event, snapshot[i].user_data);
        ++dispatched;

        if (handled && (flags & SOL_EVENT_FLAG_STOP_ON_HANDLED) != 0u) {
            break;
        }
    }

    free(snapshot);
    return dispatched;
}

/* Return a SolEventBusConfig populated with sensible defaults. */
SolEventBusConfig sol_event_bus_config_default(void)
{
    SolEventBusConfig config;
    config.initial_subscriber_capacity = 32u;
    config.initial_queue_capacity = 128u;
    return config;
}

/*
 * Allocate and initialise a new event bus.
 *
 * config   Configuration; pass NULL to use defaults.
 * Returns  Heap-allocated bus, or NULL on OOM.
 */
SolEventBus *sol_event_bus_create(const SolEventBusConfig *config)
{
    const SolEventBusConfig effective = config ? *config : sol_event_bus_config_default();

    SolEventBus *bus = (SolEventBus *)calloc(1u, sizeof(SolEventBus));
    if (!bus) {
        return NULL;
    }

    if (pthread_mutex_init(&bus->lock, NULL) != 0) {
        free(bus);
        return NULL;
    }

    bus->next_token = 1u;

    size_t subscription_capacity = effective.initial_subscriber_capacity;
    if (subscription_capacity == 0u) {
        subscription_capacity = 32u;
    }

    size_t queue_capacity = effective.initial_queue_capacity;
    if (queue_capacity == 0u) {
        queue_capacity = 128u;
    }

    bus->subscriptions = (SolSubscription *)calloc(subscription_capacity, sizeof(SolSubscription));
    bus->queue = (SolQueuedEvent *)calloc(queue_capacity, sizeof(SolQueuedEvent));

    if (!bus->subscriptions || !bus->queue) {
        free(bus->subscriptions);
        free(bus->queue);
        pthread_mutex_destroy(&bus->lock);
        free(bus);
        return NULL;
    }

    bus->subscription_capacity = subscription_capacity;
    bus->queue_capacity = queue_capacity;
    return bus;
}

/*
 * Free all resources owned by the event bus.
 *
 * Drains and frees any queued events before releasing the bus itself. Passing
 * NULL is a no-op.
 *
 * bus  Event bus to destroy.
 */
void sol_event_bus_destroy(SolEventBus *bus)
{
    if (!bus) {
        return;
    }

    for (size_t i = 0; i < bus->queue_count; ++i) {
        const size_t idx = (bus->queue_head + i) % bus->queue_capacity;
        free(bus->queue[idx].payload_copy);
        free(bus->queue[idx].name_copy);
    }

    free(bus->subscriptions);
    free(bus->queue);
    pthread_mutex_destroy(&bus->lock);
    free(bus);
}

/*
 * Derive a stable numeric event type from a string name using FNV-1a hashing.
 *
 * name     Event name string; NULL or empty returns 0.
 * Returns  64-bit hash value used as the event type id.
 */
SolEventType sol_event_type_from_name(const char *name)
{
    if (!name || *name == '\0') {
        return 0u;
    }

    uint64_t hash = 1469598103934665603ull;
    while (*name != '\0') {
        hash ^= (unsigned char)(*name);
        hash *= 1099511628211ull;
        ++name;
    }

    return hash;
}

/*
 * Register a handler and return a token that can later cancel the subscription.
 *
 * Subscribers are kept sorted by descending priority so higher-priority
 * handlers always fire first.
 *
 * bus      Event bus to register with.
 * desc     Subscription descriptor (handler, priority, event type/name).
 * Returns  Non-zero token on success, 0 on failure.
 */
SolSubscriptionToken sol_event_bus_subscribe(
    SolEventBus *bus,
    const SolEventSubscriptionDesc *desc
)
{
    if (!bus || !desc || !desc->handler) {
        return 0u;
    }

    SolSubscription subscription;
    subscription.token = 0u;
    subscription.event_type = sol_resolve_type(desc->event_type, desc->event_name);
    subscription.priority = desc->priority;
    subscription.handler = desc->handler;
    subscription.user_data = desc->user_data;
    subscription.active = true;

    pthread_mutex_lock(&bus->lock);

    if (!sol_event_bus_reserve_subscriptions(bus, bus->subscription_count + 1u)) {
        pthread_mutex_unlock(&bus->lock);
        return 0u;
    }

    subscription.token = bus->next_token++;

    size_t insert_at = bus->subscription_count;
    for (size_t i = 0; i < bus->subscription_count; ++i) {
        if (subscription.priority > bus->subscriptions[i].priority) {
            insert_at = i;
            break;
        }
    }

    for (size_t i = bus->subscription_count; i > insert_at; --i) {
        bus->subscriptions[i] = bus->subscriptions[i - 1u];
    }

    bus->subscriptions[insert_at] = subscription;
    ++bus->subscription_count;

    pthread_mutex_unlock(&bus->lock);
    return subscription.token;
}

/*
 * Cancel a subscription identified by its token.
 *
 * bus      Event bus the subscription belongs to.
 * token    Token returned by sol_event_bus_subscribe.
 * Returns  true if the subscription was found and removed.
 */
bool sol_event_bus_unsubscribe(SolEventBus *bus, SolSubscriptionToken token)
{
    if (!bus || token == 0u) {
        return false;
    }

    bool found = false;
    pthread_mutex_lock(&bus->lock);
    for (size_t i = 0; i < bus->subscription_count; ++i) {
        if (bus->subscriptions[i].token != token) {
            continue;
        }

        bus->subscriptions[i].active = false;
        found = true;
        break;
    }

    if (found) {
        sol_event_bus_compact_locked(bus);
    }

    pthread_mutex_unlock(&bus->lock);
    return found;
}

/*
 * Publish an event synchronously, invoking all matching handlers immediately.
 *
 * bus      Event bus to publish through.
 * desc     Event descriptor (name/type, payload, sender, flags).
 * Returns  Number of handlers that were called.
 */
size_t sol_event_bus_publish(SolEventBus *bus, const SolEventDesc *desc)
{
    if (!bus || !desc) {
        return 0u;
    }

    SolEvent event;
    event.type = sol_resolve_type(desc->event_type, desc->event_name);
    event.name = desc->event_name;
    event.payload = desc->payload;
    event.payload_size = desc->payload_size;
    event.sender = desc->sender;
    event.timestamp_ns = sol_now_ns();

    return sol_event_bus_dispatch(bus, &event, desc->flags);
}

/*
 * Enqueue an event for deferred dispatch on the next sol_event_bus_drain call.
 *
 * Copies the payload and name so the caller may free them immediately.
 *
 * bus      Event bus to post to.
 * desc     Event descriptor.
 * Returns  true if the event was enqueued successfully.
 */
bool sol_event_bus_post(SolEventBus *bus, const SolEventDesc *desc)
{
    if (!bus || !desc) {
        return false;
    }

    SolQueuedEvent queued;
    memset(&queued, 0, sizeof(queued));

    queued.event.type = sol_resolve_type(desc->event_type, desc->event_name);
    queued.event.sender = desc->sender;
    queued.event.timestamp_ns = sol_now_ns();
    queued.flags = desc->flags;

    if (desc->payload && desc->payload_size > 0u) {
        queued.payload_copy = malloc(desc->payload_size);
        if (!queued.payload_copy) {
            return false;
        }

        memcpy(queued.payload_copy, desc->payload, desc->payload_size);
        queued.event.payload = queued.payload_copy;
        queued.event.payload_size = desc->payload_size;
    }

    if (desc->event_name) {
        queued.name_copy = sol_strdup(desc->event_name);
        if (!queued.name_copy) {
            free(queued.payload_copy);
            return false;
        }

        queued.event.name = queued.name_copy;
    }

    pthread_mutex_lock(&bus->lock);

    if (!sol_event_bus_reserve_queue(bus, bus->queue_count + 1u)) {
        pthread_mutex_unlock(&bus->lock);
        free(queued.payload_copy);
        free(queued.name_copy);
        return false;
    }

    bus->queue[bus->queue_tail] = queued;
    bus->queue_tail = (bus->queue_tail + 1u) % bus->queue_capacity;
    ++bus->queue_count;

    pthread_mutex_unlock(&bus->lock);
    return true;
}

/*
 * Dispatch queued events, up to max_events (0 means no limit).
 *
 * Each event is dequeued under the lock, dispatched outside it, then its
 * heap-allocated copies are freed.
 *
 * bus        Event bus to drain.
 * max_events Maximum number of events to dispatch; 0 dispatches all.
 * Returns    Number of events dispatched.
 */
size_t sol_event_bus_drain(SolEventBus *bus, size_t max_events)
{
    if (!bus) {
        return 0u;
    }

    const size_t limit = max_events == 0u ? SIZE_MAX : max_events;
    size_t drained = 0u;

    while (drained < limit) {
        SolQueuedEvent queued;
        memset(&queued, 0, sizeof(queued));

        pthread_mutex_lock(&bus->lock);
        if (bus->queue_count == 0u) {
            pthread_mutex_unlock(&bus->lock);
            break;
        }

        queued = bus->queue[bus->queue_head];
        memset(&bus->queue[bus->queue_head], 0, sizeof(SolQueuedEvent));

        bus->queue_head = (bus->queue_head + 1u) % bus->queue_capacity;
        --bus->queue_count;
        pthread_mutex_unlock(&bus->lock);

        sol_event_bus_dispatch(bus, &queued.event, queued.flags);

        free(queued.payload_copy);
        free(queued.name_copy);
        ++drained;
    }

    return drained;
}
