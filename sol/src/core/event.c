#include "sol_event.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   INTERNAL TYPES
   ============================================================ */

typedef struct {
    Sol_ListenerID   id;
    Sol_EventHandler handler;
    void            *user_data;
    bool             active; /* false = pending removal */
} Listener;

/* A dynamic array of listeners for one event type. */
typedef struct {
    Listener *items;
    uint32_t  count;
    uint32_t  capacity;
} ListenerGroup;

/* Maps a custom event name to its assigned type ID. */
typedef struct {
    char         *name; /* heap-allocated copy */
    Sol_EventType type;
} CustomTypeEntry;

/* ============================================================
   BUS STATE
   ============================================================ */

/* Hard upper limit on the number of distinct event types (builtin + custom). */
#define SOL_MAX_EVENT_TYPES 512u

typedef struct {
    /* Listener groups — groups[i] holds all listeners for event type i. */
    ListenerGroup groups[SOL_MAX_EVENT_TYPES];
    uint32_t      group_count; /* number of groups currently allocated */

    /* Custom event type registry */
    CustomTypeEntry *custom_types;
    uint32_t         custom_count;
    uint32_t         custom_cap;

    /* Monotonically increasing ID handed out to new listeners. */
    Sol_ListenerID next_id;

    /* Nesting depth of active sol_event_emit calls.
       When > 0, listener removals are deferred (marked inactive) rather than
       applied immediately, so in-progress iterations are not disrupted. */
    int dispatch_depth;
} EventBus;

static EventBus g_bus;

/* ============================================================
   LISTENER GROUP HELPERS
   ============================================================ */

static void lg_push(ListenerGroup *g, Listener l)
{
    if (g->count == g->capacity) {
        g->capacity = g->capacity ? g->capacity * 2u : 4u;
        g->items = realloc(g->items, g->capacity * sizeof(Listener));
        assert(g->items && "sol: listener group allocation failed");
    }
    g->items[g->count++] = l;
}

static void lg_free(ListenerGroup *g)
{
    free(g->items);
    g->items    = NULL;
    g->count    = 0;
    g->capacity = 0;
}

/* Remove all listeners marked inactive from a group. */
static void lg_compact(ListenerGroup *g)
{
    uint32_t write = 0;
    for (uint32_t read = 0; read < g->count; read++) {
        if (g->items[read].active) {
            g->items[write++] = g->items[read];
        }
    }
    g->count = write;
}

/* ============================================================
   INIT / SHUTDOWN
   ============================================================ */

void sol_events_init(void)
{
    memset(&g_bus, 0, sizeof(g_bus));
    g_bus.group_count = (uint32_t)SOL_EVENT__BUILTIN_COUNT;
    g_bus.next_id     = 1u;
}

void sol_events_shutdown(void)
{
    for (uint32_t i = 0; i < g_bus.group_count; i++) {
        lg_free(&g_bus.groups[i]);
    }
    for (uint32_t i = 0; i < g_bus.custom_count; i++) {
        free(g_bus.custom_types[i].name);
    }
    free(g_bus.custom_types);
    memset(&g_bus, 0, sizeof(g_bus));
}

/* ============================================================
   SUBSCRIBE / UNSUBSCRIBE
   ============================================================ */

Sol_ListenerID sol_event_on(Sol_EventType type, Sol_EventHandler handler,
                            void *user_data)
{
    if (handler == NULL) return SOL_INVALID_LISTENER;
    if (type == SOL_EVENT_NONE || type >= g_bus.group_count) {
        return SOL_INVALID_LISTENER;
    }

    Listener l = {
        .id        = g_bus.next_id++,
        .handler   = handler,
        .user_data = user_data,
        .active    = true,
    };
    lg_push(&g_bus.groups[type], l);
    return l.id;
}

void sol_event_off(Sol_ListenerID id)
{
    if (id == SOL_INVALID_LISTENER) return;

    for (uint32_t g = 0; g < g_bus.group_count; g++) {
        ListenerGroup *group = &g_bus.groups[g];
        for (uint32_t i = 0; i < group->count; i++) {
            if (group->items[i].id != id) continue;

            if (g_bus.dispatch_depth > 0) {
                /* Deferred removal: mark inactive so ongoing iteration is safe. */
                group->items[i].active = false;
            } else {
                /* Immediate removal preserving insertion order. */
                memmove(&group->items[i], &group->items[i + 1],
                        (group->count - i - 1u) * sizeof(Listener));
                group->count--;
            }
            return;
        }
    }
}

/* ============================================================
   EMIT
   ============================================================ */

/*
 * sol_event_emit accepts a pointer to one of the Sol_*Event data structs,
 * copies it into the Sol_Event union, then dispatches to all active listeners.
 *
 * Passing NULL for `data` is valid for events that carry no payload (e.g.
 * SOL_EVENT_APP_QUIT).
 */
void sol_event_emit(Sol_EventType type, const void *data)
{
    if (type == SOL_EVENT_NONE || type >= g_bus.group_count) return;

    Sol_Event event;
    memset(&event, 0, sizeof(event));
    event.type     = type;
    event.consumed = false;

    /* Copy the payload into the union.  Built-in payloads all fit inside the
       union; for custom events the caller stores a pointer in custom_data. */
    if (data) {
        /* Compute the payload size based on type.  For custom events we copy
           only the pointer itself — the caller owns the backing storage. */
        static const size_t payload_sizes[SOL_EVENT__BUILTIN_COUNT] = {
            [SOL_EVENT_NONE]               = 0,
            [SOL_EVENT_APP_INIT]           = 0,
            [SOL_EVENT_APP_QUIT]           = 0,
            [SOL_EVENT_APP_TICK]           = 0,
            [SOL_EVENT_WINDOW_RESIZE]      = sizeof(Sol_ResizeEvent),
            [SOL_EVENT_WINDOW_FOCUS_GAINED]= 0,
            [SOL_EVENT_WINDOW_FOCUS_LOST]  = 0,
            [SOL_EVENT_KEY]                = sizeof(Sol_KeyEvent),
            [SOL_EVENT_CHAR]               = sizeof(Sol_CharEvent),
            [SOL_EVENT_MOUSE_BUTTON]       = sizeof(Sol_MouseButtonEvent),
            [SOL_EVENT_MOUSE_MOVE]         = sizeof(Sol_MouseMoveEvent),
            [SOL_EVENT_MOUSE_SCROLL]       = sizeof(Sol_MouseScrollEvent),
            [SOL_EVENT_BUFFER_OPEN]        = sizeof(Sol_BufferEvent),
            [SOL_EVENT_BUFFER_CLOSE]       = sizeof(Sol_BufferEvent),
            [SOL_EVENT_BUFFER_SAVE]        = sizeof(Sol_BufferEvent),
            [SOL_EVENT_BUFFER_CHANGE]      = sizeof(Sol_ChangeEvent),
            [SOL_EVENT_CURSOR_MOVE]        = sizeof(Sol_CursorEvent),
            [SOL_EVENT_SELECTION_CHANGE]   = sizeof(Sol_SelectionEvent),
            [SOL_EVENT_COMMAND]            = sizeof(Sol_CommandEvent),
        };

        size_t sz = (type < SOL_EVENT__BUILTIN_COUNT)
                  ? payload_sizes[type]
                  : sizeof(void *);

        if (sz > 0) {
            /* Guard: the payload must fit in the union. */
            assert(sz <= sizeof(event.custom_data) + sizeof(Sol_SelectionEvent));
            memcpy(&event.resize, data, sz); /* union starts at &event.resize */
        }
    }

    ListenerGroup *group = &g_bus.groups[type];
    g_bus.dispatch_depth++;

    for (uint32_t i = 0; i < group->count; i++) {
        Listener *l = &group->items[i];
        if (!l->active) continue;
        l->handler(&event, l->user_data);
        if (event.consumed) break;
    }

    g_bus.dispatch_depth--;

    /* Once the outermost dispatch returns, compact every group that may have
       accumulated deferred removals. */
    if (g_bus.dispatch_depth == 0) {
        for (uint32_t i = 0; i < g_bus.group_count; i++) {
            lg_compact(&g_bus.groups[i]);
        }
    }
}

/* ============================================================
   CUSTOM TYPE REGISTRY
   ============================================================ */

Sol_EventType sol_event_register_type(const char *name)
{
    assert(name != NULL);

    /* Return existing ID if already registered. */
    Sol_EventType existing = sol_event_find_type(name);
    if (existing != SOL_EVENT_NONE) return existing;

    if (g_bus.group_count >= SOL_MAX_EVENT_TYPES) {
        fprintf(stderr, "sol: event type limit (%u) reached, cannot register '%s'\n",
                SOL_MAX_EVENT_TYPES, name);
        return SOL_EVENT_NONE;
    }

    Sol_EventType type = g_bus.group_count++;
    memset(&g_bus.groups[type], 0, sizeof(ListenerGroup));

    /* Grow the name registry if needed. */
    if (g_bus.custom_count == g_bus.custom_cap) {
        g_bus.custom_cap = g_bus.custom_cap ? g_bus.custom_cap * 2u : 8u;
        g_bus.custom_types = realloc(g_bus.custom_types,
                                     g_bus.custom_cap * sizeof(CustomTypeEntry));
        assert(g_bus.custom_types && "sol: custom type registry allocation failed");
    }

    g_bus.custom_types[g_bus.custom_count++] = (CustomTypeEntry){
        .name = strdup(name),
        .type = type,
    };

    return type;
}

Sol_EventType sol_event_find_type(const char *name)
{
    assert(name != NULL);
    for (uint32_t i = 0; i < g_bus.custom_count; i++) {
        if (strcmp(g_bus.custom_types[i].name, name) == 0) {
            return g_bus.custom_types[i].type;
        }
    }
    return SOL_EVENT_NONE;
}
