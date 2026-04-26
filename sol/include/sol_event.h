#ifndef SOL_EVENT_H
#define SOL_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t SolEventType;
typedef uint64_t SolSubscriptionToken;

typedef struct SolEventBus SolEventBus;

typedef struct SolEvent {
    SolEventType type;
    const char *name;
    const void *payload;
    size_t payload_size;
    void *sender;
    uint64_t timestamp_ns;
} SolEvent;

typedef bool (*SolEventHandler)(const SolEvent *event, void *user_data);

enum {
    SOL_EVENT_FLAG_NONE = 0u,
    SOL_EVENT_FLAG_STOP_ON_HANDLED = 1u << 0,
};

typedef struct SolEventDesc {
    SolEventType event_type;
    const char *event_name;
    const void *payload;
    size_t payload_size;
    void *sender;
    uint32_t flags;
} SolEventDesc;

typedef struct SolEventSubscriptionDesc {
    SolEventType event_type;
    const char *event_name;
    int priority;
    SolEventHandler handler;
    void *user_data;
} SolEventSubscriptionDesc;

typedef struct SolEventBusConfig {
    size_t initial_subscriber_capacity;
    size_t initial_queue_capacity;
} SolEventBusConfig;

SolEventBusConfig sol_event_bus_config_default(void);

SolEventBus *sol_event_bus_create(const SolEventBusConfig *config);
void sol_event_bus_destroy(SolEventBus *bus);

SolEventType sol_event_type_from_name(const char *name);

SolSubscriptionToken sol_event_bus_subscribe(
    SolEventBus *bus,
    const SolEventSubscriptionDesc *desc
);

bool sol_event_bus_unsubscribe(SolEventBus *bus, SolSubscriptionToken token);

size_t sol_event_bus_publish(SolEventBus *bus, const SolEventDesc *desc);

bool sol_event_bus_post(SolEventBus *bus, const SolEventDesc *desc);

size_t sol_event_bus_drain(SolEventBus *bus, size_t max_events);

#endif
