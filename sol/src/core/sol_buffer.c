#include "sol_buffer.h"

#include <causality.h>   /* Ca_Signal, ca_signal_set_u32, ca_signal_get_u32 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sol_event.h"

typedef enum SolLayoutNodeType {
    SOL_LAYOUT_NODE_LEAF = 0,
    SOL_LAYOUT_NODE_SPLIT,
} SolLayoutNodeType;

typedef struct SolLayoutNode {
    SolBufferNodeId id;
    SolBufferNodeId parent_id;
    bool in_use;
    SolLayoutNodeType type;
    union {
        struct {
            SolBufferId buffer_id;
            SolBufferId *tabs;
            size_t tab_count;
            size_t tab_capacity;
            size_t tab_view_start;
            SolBufferId tab_last_active_id;
            /* Per-pane scroll state — independent even when the same
             * buffer is open in multiple leaves simultaneously. */
            int scroll_top;
            int scroll_left;
        } leaf;
        struct {
            SolBufferSplitDirection direction;
            float ratio;
            SolBufferNodeId first_id;
            SolBufferNodeId second_id;
        } split;
    } as;
} SolLayoutNode;

struct SolBuffer {
    SolBufferId id;
    char *name;
    SolBufferKind kind;
    void *state;
    SolBufferOps ops;
    bool in_use;
};

struct SolBufferSystem {
    SolBuffer *buffers;
    size_t buffer_count;
    size_t buffer_capacity;
    SolBufferId next_buffer_id;

    SolLayoutNode *nodes;
    size_t node_count;
    size_t node_capacity;
    SolBufferNodeId next_node_id;

    SolBufferNodeId root_id;
    SolBufferNodeId active_leaf_id;

    /* Optional caller-owned u32 revision signal. Bumped by bump_rev()
       on every successful state mutation. NULL until attached. */
    Ca_Signal *rev;

    /* Optional caller-owned event bus. When attached, the system
       publishes sol.buffer.opened / closed / focused on the
       corresponding mutations. NULL until attached. */
    SolEventBus *events;

    /* Tracks which buffer was active at the end of the previous
       focus-bumping mutation so we only publish sol.buffer.focused
       on an actual change. Updated unconditionally (not gated on the
       event bus) so the focus-history is always consistent. */
    SolBufferId  last_focused_buffer_id;

    /* The buffer that was active immediately BEFORE the current one.
       Drives `sol_buffer_focus_previous_buffer` (Ctrl+B,B). 0 until
       at least one focus change has occurred. */
    SolBufferId  previous_focused_buffer_id;
};

static const SolBuffer *sol_buffer_find_const(const SolBufferSystem *system, SolBufferId id);

static bool sol_leaf_add_tab(SolLayoutNode *leaf, SolBufferId buffer_id)
{
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF || buffer_id == 0u)
        return false;
    for (size_t i = 0u; i < leaf->as.leaf.tab_count; i++)
        if (leaf->as.leaf.tabs[i] == buffer_id) return true;
    if (leaf->as.leaf.tab_count == leaf->as.leaf.tab_capacity) {
        size_t capacity = leaf->as.leaf.tab_capacity
            ? leaf->as.leaf.tab_capacity * 2u : 4u;
        SolBufferId *tabs = (SolBufferId *)realloc(
            leaf->as.leaf.tabs, capacity * sizeof(SolBufferId));
        if (!tabs) return false;
        leaf->as.leaf.tabs = tabs;
        leaf->as.leaf.tab_capacity = capacity;
    }
    leaf->as.leaf.tabs[leaf->as.leaf.tab_count++] = buffer_id;
    return true;
}

static bool sol_leaf_remove_tab(SolLayoutNode *leaf, SolBufferId buffer_id)
{
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return false;
    size_t index = leaf->as.leaf.tab_count;
    for (size_t i = 0u; i < leaf->as.leaf.tab_count; i++)
        if (leaf->as.leaf.tabs[i] == buffer_id) { index = i; break; }
    if (index == leaf->as.leaf.tab_count) return false;

    if (leaf->as.leaf.buffer_id == buffer_id) {
        leaf->as.leaf.buffer_id = index > 0u
            ? leaf->as.leaf.tabs[index - 1u]
            : (index + 1u < leaf->as.leaf.tab_count
                ? leaf->as.leaf.tabs[index + 1u] : 0u);
    }
    memmove(&leaf->as.leaf.tabs[index], &leaf->as.leaf.tabs[index + 1u],
            (leaf->as.leaf.tab_count - index - 1u) * sizeof(SolBufferId));
    leaf->as.leaf.tab_count--;
    if (leaf->as.leaf.tab_view_start > leaf->as.leaf.tab_count)
        leaf->as.leaf.tab_view_start = leaf->as.leaf.tab_count;
    return true;
}

static bool sol_buffer_has_tab(const SolBufferSystem *system,
                               SolBufferId buffer_id)
{
    if (!system) return false;
    for (size_t i = 0u; i < system->node_count; i++) {
        const SolLayoutNode *node = &system->nodes[i];
        if (!node->in_use || node->type != SOL_LAYOUT_NODE_LEAF) continue;
        for (size_t j = 0u; j < node->as.leaf.tab_count; j++)
            if (node->as.leaf.tabs[j] == buffer_id) return true;
    }
    return false;
}

/* Track focus history and publish sol.buffer.focused when the active
   buffer changes. Safe to call from every focus-touching mutator;
   history is updated even when no event bus is attached. */
static void maybe_publish_focus(SolBufferSystem *system)
{
    if (!system) return;
    const SolBufferId current = sol_buffer_active_buffer(system);
    if (current == system->last_focused_buffer_id) return;

    /* Slide the history window: previous <- last, last <- current.
       Only record a non-zero prior as `previous` so transient empty
       leaves (buffer_id == 0) do not poison the back-target. */
    if (system->last_focused_buffer_id != 0u) {
        system->previous_focused_buffer_id = system->last_focused_buffer_id;
    }
    system->last_focused_buffer_id = current;

    if (!system->events || current == 0u) return;

    const SolBuffer *buf = sol_buffer_find_const(system, current);
    if (!buf) return;
    SolBufferEventPayload payload;
    payload.buffer_id   = current;
    payload.kind        = buf->kind;
    payload.name        = buf->name;
    payload.source_path = NULL;   /* unknown at this layer */
    sol_event_publish(system->events, SOL_EVENT_BUFFER_FOCUSED,
                       &payload, sizeof(payload), system);
}

/* Single point of notification — every mutator that changes
   user-visible state calls this once on success. Cheap when no signal
   is attached, idiomatic causality when one is. Also forwards focus
   changes to the event bus when one is attached. */
static void bump_rev(SolBufferSystem *system)
{
    if (!system) return;
    if (system->rev) {
        ca_signal_set_u32(system->rev, ca_signal_get_u32(system->rev) + 1u);
    }
    maybe_publish_focus(system);
}

/*
 * Duplicate a string into a heap-allocated buffer.
 *
 * value   Source string to copy; NULL is accepted and returns NULL.
 * Returns Heap-allocated copy, or NULL on allocation failure or NULL input.
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

/* Clamp a split ratio to [0.1, 0.9] to prevent unusable pane sizes. */
static float sol_clamp_ratio(float ratio)
{
    if (ratio < 0.1f) {
        return 0.1f;
    }
    if (ratio > 0.9f) {
        return 0.9f;
    }
    return ratio;
}

/*
 * Grow the buffer slot array to hold at least min_capacity entries.
 *
 * system        Buffer system whose array is grown.
 * min_capacity  Minimum required capacity.
 * Returns       true on success or if capacity is already sufficient.
 */
static bool sol_buffer_reserve_buffers(SolBufferSystem *system, size_t min_capacity)
{
    if (system->buffer_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = system->buffer_capacity == 0u ? 16u : system->buffer_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolBuffer *new_items = (SolBuffer *)realloc(system->buffers, new_capacity * sizeof(SolBuffer));
    if (!new_items) {
        return false;
    }

    system->buffers = new_items;
    system->buffer_capacity = new_capacity;
    return true;
}

/*
 * Grow the layout node array to hold at least min_capacity entries.
 *
 * system        Buffer system whose node array is grown.
 * min_capacity  Minimum required capacity.
 * Returns       true on success or if capacity is already sufficient.
 */
static bool sol_buffer_reserve_nodes(SolBufferSystem *system, size_t min_capacity)
{
    if (system->node_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = system->node_capacity == 0u ? 16u : system->node_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolLayoutNode *new_items = (SolLayoutNode *)realloc(system->nodes, new_capacity * sizeof(SolLayoutNode));
    if (!new_items) {
        return false;
    }

    system->nodes = new_items;
    system->node_capacity = new_capacity;
    return true;
}

/*
 * Find a live buffer by id within the system.
 *
 * system  Buffer system to search.
 * id      Buffer id to look up.
 * Returns Pointer to the matching buffer slot, or NULL if not found.
 */
static SolBuffer *sol_buffer_find(SolBufferSystem *system, SolBufferId id)
{
    if (!system || id == 0u) {
        return NULL;
    }

    for (size_t i = 0u; i < system->buffer_count; ++i) {
        if (system->buffers[i].in_use && system->buffers[i].id == id) {
            return &system->buffers[i];
        }
    }

    return NULL;
}

static const SolBuffer *sol_buffer_find_const(const SolBufferSystem *system, SolBufferId id)
{
    return sol_buffer_find((SolBufferSystem *)system, id);
}

/*
 * Find a live layout node by id within the system.
 *
 * system  Buffer system to search.
 * id      Node id to look up.
 * Returns Pointer to the matching node, or NULL if not found.
 */
static SolLayoutNode *sol_layout_find_node(SolBufferSystem *system, SolBufferNodeId id)
{
    if (!system || id == 0u) {
        return NULL;
    }

    for (size_t i = 0u; i < system->node_count; ++i) {
        if (system->nodes[i].in_use && system->nodes[i].id == id) {
            return &system->nodes[i];
        }
    }

    return NULL;
}

static const SolLayoutNode *sol_layout_find_node_const(const SolBufferSystem *system, SolBufferNodeId id)
{
    return sol_layout_find_node((SolBufferSystem *)system, id);
}

/*
 * Allocate a new leaf node that references the given buffer.
 *
 * system     Buffer system that owns the node pool.
 * buffer_id  Buffer to associate with the leaf (0 = empty leaf).
 * Returns    Pointer to the new node, or NULL on allocation failure.
 */
static SolLayoutNode *sol_layout_create_leaf(SolBufferSystem *system, SolBufferId buffer_id)
{
    if (!sol_buffer_reserve_nodes(system, system->node_count + 1u)) {
        return NULL;
    }

    SolLayoutNode node;
    memset(&node, 0, sizeof(node));
    node.id = system->next_node_id++;
    node.in_use = true;
    node.type = SOL_LAYOUT_NODE_LEAF;
    node.as.leaf.buffer_id = buffer_id;
    if (buffer_id != 0u && !sol_leaf_add_tab(&node, buffer_id)) return NULL;

    system->nodes[system->node_count++] = node;
    return &system->nodes[system->node_count - 1u];
}

/*
 * Allocate a new split node that divides two child nodes.
 *
 * system     Buffer system that owns the node pool.
 * direction  Whether the split is vertical or horizontal.
 * ratio      Fraction of space given to the first child; clamped to [0.1, 0.9].
 * first_id   Node id of the first (left/top) child.
 * second_id  Node id of the second (right/bottom) child.
 * Returns    Pointer to the new split node, or NULL on allocation failure.
 */
static SolLayoutNode *sol_layout_create_split(
    SolBufferSystem *system,
    SolBufferSplitDirection direction,
    float ratio,
    SolBufferNodeId first_id,
    SolBufferNodeId second_id
)
{
    if (!sol_buffer_reserve_nodes(system, system->node_count + 1u)) {
        return NULL;
    }

    SolLayoutNode node;
    memset(&node, 0, sizeof(node));
    node.id = system->next_node_id++;
    node.in_use = true;
    node.type = SOL_LAYOUT_NODE_SPLIT;
    node.as.split.direction = direction;
    node.as.split.ratio = sol_clamp_ratio(ratio);
    node.as.split.first_id = first_id;
    node.as.split.second_id = second_id;

    system->nodes[system->node_count++] = node;
    return &system->nodes[system->node_count - 1u];
}

/*
 * Return the id of the first live buffer in registration order.
 *
 * system  Buffer system to query.
 * Returns Id of the first live buffer, or 0 if none exist.
 */
static SolBufferId sol_first_live_buffer_id(const SolBufferSystem *system)
{
    if (!system) {
        return 0u;
    }

    for (size_t i = 0u; i < system->buffer_count; ++i) {
        if (system->buffers[i].in_use) {
            return system->buffers[i].id;
        }
    }

    return 0u;
}

/*
 * Collect leaf node ids in left-to-right DFS order into out_ids[].
 *
 * system   Buffer system owning the layout tree.
 * node_id  Root of the subtree to traverse.
 * out_ids  Caller-allocated array to receive leaf ids.
 * cursor   Current write position in out_ids; incremented for each leaf found.
 */
static void sol_collect_leaf_ids(const SolBufferSystem *system, SolBufferNodeId node_id, SolBufferNodeId *out_ids, size_t *cursor)
{
    const SolLayoutNode *node = sol_layout_find_node_const(system, node_id);
    if (!node || !out_ids || !cursor) {
        return;
    }

    if (node->type == SOL_LAYOUT_NODE_LEAF) {
        out_ids[*cursor] = node->id;
        *cursor += 1u;
        return;
    }

    sol_collect_leaf_ids(system, node->as.split.first_id, out_ids, cursor);
    sol_collect_leaf_ids(system, node->as.split.second_id, out_ids, cursor);
}

/*
 * Return a SolBufferSystemConfig populated with sensible defaults.
 *
 * Returns  Config struct with initial_buffer_capacity = 32 and
 *          initial_node_capacity = 32.
 */
SolBufferSystemConfig sol_buffer_system_config_default(void)
{
    SolBufferSystemConfig config;
    config.initial_buffer_capacity = 32u;
    config.initial_node_capacity = 32u;
    return config;
}

/*
 * Create and initialise a new buffer system.
 *
 * config  Optional configuration; NULL uses sol_buffer_system_config_default().
 * Returns Heap-allocated system, or NULL on allocation failure.
 */
SolBufferSystem *sol_buffer_system_create(const SolBufferSystemConfig *config)
{
    SolBufferSystemConfig effective = config ? *config : sol_buffer_system_config_default();

    SolBufferSystem *system = (SolBufferSystem *)calloc(1u, sizeof(SolBufferSystem));
    if (!system) {
        return NULL;
    }

    system->next_buffer_id = 1u;
    system->next_node_id = 1u;

    system->buffer_capacity = effective.initial_buffer_capacity == 0u ? 32u : effective.initial_buffer_capacity;
    system->node_capacity = effective.initial_node_capacity == 0u ? 32u : effective.initial_node_capacity;

    system->buffers = (SolBuffer *)calloc(system->buffer_capacity, sizeof(SolBuffer));
    system->nodes = (SolLayoutNode *)calloc(system->node_capacity, sizeof(SolLayoutNode));

    if (!system->buffers || !system->nodes) {
        free(system->buffers);
        free(system->nodes);
        free(system);
        return NULL;
    }

    return system;
}

/*
 * Destroy all buffers and free the system.
 *
 * system  System to destroy; NULL is ignored.
 */
void sol_buffer_system_destroy(SolBufferSystem *system)
{
    if (!system) {
        return;
    }

    for (size_t i = 0u; i < system->buffer_count; ++i) {
        SolBuffer *buffer = &system->buffers[i];
        if (!buffer->in_use) {
            continue;
        }

        if (buffer->ops.destroy) {
            buffer->ops.destroy(buffer->state);
        }

        free(buffer->name);
        memset(buffer, 0, sizeof(*buffer));
    }

    for (size_t i = 0u; i < system->node_count; i++) {
        if (system->nodes[i].type == SOL_LAYOUT_NODE_LEAF)
            free(system->nodes[i].as.leaf.tabs);
    }

    free(system->buffers);
    free(system->nodes);
    free(system);
}

void sol_buffer_attach_revision_signal(SolBufferSystem *system, Ca_Signal *sig)
{
    if (!system) {
        return;
    }
    system->rev = sig;
}

void sol_buffer_attach_event_bus(SolBufferSystem *system, SolEventBus *bus)
{
    if (!system) return;
    system->events = bus;
}

SolEventBus *sol_buffer_event_bus(SolBufferSystem *system)
{
    return system ? system->events : NULL;
}

/*
 * Create a new buffer from a descriptor and register it with the system.
 *
 * system  Buffer system to register the buffer in.
 * desc    Descriptor with name, kind, state pointer, and ops vtable.
 * Returns Id of the newly created buffer, or 0 on failure.
 */
SolBufferId sol_buffer_create(SolBufferSystem *system, const SolBufferDesc *desc)
{
    if (!system || !desc) {
        return 0u;
    }

    const bool had_live_buffers = sol_first_live_buffer_id(system) != 0u;
    const char *name = desc->name ? desc->name : "[No Name]";
    char *name_copy = sol_strdup(name);
    if (!name_copy) {
        return 0u;
    }

    if (!sol_buffer_reserve_buffers(system, system->buffer_count + 1u)) {
        free(name_copy);
        return 0u;
    }

    SolBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.id = system->next_buffer_id++;
    buffer.name = name_copy;
    buffer.kind = desc->kind;
    buffer.state = desc->state;
    buffer.ops = desc->ops;
    buffer.in_use = true;

    system->buffers[system->buffer_count++] = buffer;

    if (system->root_id == 0u) {
        SolLayoutNode *leaf = sol_layout_create_leaf(system, buffer.id);
        if (!leaf) {
            if (buffer.ops.destroy) {
                buffer.ops.destroy(buffer.state);
            }
            free(name_copy);
            --system->buffer_count;
            return 0u;
        }

        system->root_id = leaf->id;
        system->active_leaf_id = leaf->id;
    } else if (!had_live_buffers) {
        /* Closing all buffers preserves the split tree. Reuse its active leaf
         * when editing resumes instead of leaving the new buffer invisible. */
        SolLayoutNode *leaf = sol_layout_find_node(
            system, system->active_leaf_id);
        if (leaf && leaf->type == SOL_LAYOUT_NODE_LEAF) {
            (void)sol_leaf_add_tab(leaf, buffer.id);
            leaf->as.leaf.buffer_id = buffer.id;
        }
    }

    if (system->events) {
        SolBufferEventPayload payload;
        payload.buffer_id   = buffer.id;
        payload.kind        = buffer.kind;
        payload.name        = buffer.name;
        payload.source_path = NULL;
        sol_event_publish(system->events, SOL_EVENT_BUFFER_OPENED,
                           &payload, sizeof(payload), system);
    }
    bump_rev(system);   /* also publishes sol.buffer.focused if active changed */
    return buffer.id;
}

/*
 * Close and free a buffer, redirecting any leaf that showed it to a neighbor.
 *
 * system     Buffer system owning the buffer.
 * buffer_id  Id of the buffer to close.
 * Returns    true if the buffer was found and closed.
 */
bool sol_buffer_close(SolBufferSystem *system, SolBufferId buffer_id)
{
    SolBuffer *buffer = sol_buffer_find(system, buffer_id);
    if (!buffer) {
        return false;
    }

    /* Snapshot for the CLOSED event before we tear the buffer down. */
    SolBufferEventPayload closed_payload;
    closed_payload.buffer_id   = buffer->id;
    closed_payload.kind        = buffer->kind;
    closed_payload.name        = buffer->name;
    closed_payload.source_path = NULL;
    if (system->events) {
        sol_event_publish(system->events, SOL_EVENT_BUFFER_CLOSED,
                           &closed_payload, sizeof(closed_payload), system);
    }

    if (buffer->ops.destroy) {
        buffer->ops.destroy(buffer->state);
    }

    free(buffer->name);
    buffer->name = NULL;
    buffer->state = NULL;
    buffer->in_use = false;

    for (size_t i = 0u; i < system->node_count; i++) {
        SolLayoutNode *node = &system->nodes[i];
        if (node->in_use && node->type == SOL_LAYOUT_NODE_LEAF)
            (void)sol_leaf_remove_tab(node, buffer_id);
    }

    bump_rev(system);   /* also publishes sol.buffer.focused if active changed */
    return true;
}

size_t sol_buffer_close_all(SolBufferSystem *system)
{
    if (!system) return 0u;

    size_t closed = 0u;
    SolBufferId id;
    while ((id = sol_buffer_at(system, 0u)) != 0u) {
        if (!sol_buffer_close(system, id)) break;
        closed++;
    }
    return closed;
}

SolBuffer *sol_buffer_get(SolBufferSystem *system, SolBufferId buffer_id)
{
    return sol_buffer_find(system, buffer_id);
}

const SolBuffer *sol_buffer_get_const(const SolBufferSystem *system, SolBufferId buffer_id)
{
    return sol_buffer_find_const(system, buffer_id);
}

const char *sol_buffer_name(const SolBuffer *buffer)
{
    return buffer ? buffer->name : NULL;
}

SolBufferKind sol_buffer_kind(const SolBuffer *buffer)
{
    return buffer ? buffer->kind : SOL_BUFFER_KIND_CUSTOM;
}

SolBufferId sol_buffer_id(const SolBuffer *buffer)
{
    return buffer ? buffer->id : 0u;
}

void *sol_buffer_state(SolBuffer *buffer)
{
    return buffer ? buffer->state : NULL;
}

const void *sol_buffer_state_const(const SolBuffer *buffer)
{
    return buffer ? buffer->state : NULL;
}

/*
 * Split the active leaf pane and place new_buffer_id in the new pane.
 *
 * system          Buffer system to modify.
 * direction       SOL_BUFFER_SPLIT_VERTICAL or SOL_BUFFER_SPLIT_HORIZONTAL.
 * ratio           Fraction of space given to the existing pane; clamped to [0.1, 0.9].
 * new_buffer_id   Buffer to display in the new pane (0 = empty pane).
 * out_new_leaf_id If non-NULL, receives the node id of the newly created leaf.
 * Returns         true on success.
 */
bool sol_buffer_split_active(
    SolBufferSystem *system,
    SolBufferSplitDirection direction,
    float ratio,
    SolBufferId new_buffer_id,
    SolBufferNodeId *out_new_leaf_id
)
{
    if (!system || system->active_leaf_id == 0u) {
        return false;
    }

    SolLayoutNode *active_leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!active_leaf || active_leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }

    /* new_buffer_id == 0u creates an EMPTY leaf (no buffer assigned).
       Callers that want to mirror the active leaf's buffer pass it
       explicitly. The empty leaf renders as a blank pane until a
       buffer is opened into it. */
    if (new_buffer_id != 0u && !sol_buffer_find(system, new_buffer_id)) {
        return false;
    }

    const SolBufferNodeId active_id = active_leaf->id;
    const SolBufferNodeId parent_id = active_leaf->parent_id;

    SolLayoutNode *new_leaf = sol_layout_create_leaf(system, new_buffer_id);
    if (!new_leaf) {
        return false;
    }

    const SolBufferNodeId new_leaf_id = new_leaf->id;
    SolLayoutNode *split = sol_layout_create_split(system, direction, ratio,
                                                    active_id, new_leaf_id);
    if (!split) {
        free(new_leaf->as.leaf.tabs);
        new_leaf->as.leaf.tabs = NULL;
        new_leaf->in_use = false;
        return false;
    }

    active_leaf = sol_layout_find_node(system, active_id);
    new_leaf = sol_layout_find_node(system, new_leaf_id);
    if (!active_leaf || !new_leaf) return false;
    split->parent_id = parent_id;
    active_leaf->parent_id = split->id;
    new_leaf->parent_id = split->id;

    if (parent_id == 0u) {
        system->root_id = split->id;
    } else {
        SolLayoutNode *parent = sol_layout_find_node(system, parent_id);
        if (!parent || parent->type != SOL_LAYOUT_NODE_SPLIT) {
            return false;
        }

        if (parent->as.split.first_id == active_leaf->id) {
            parent->as.split.first_id = split->id;
        } else if (parent->as.split.second_id == active_leaf->id) {
            parent->as.split.second_id = split->id;
        } else {
            return false;
        }
    }

    system->active_leaf_id = new_leaf->id;
    if (out_new_leaf_id) {
        *out_new_leaf_id = new_leaf->id;
    }

    bump_rev(system);
    return true;
}

/*
 * Shift focus to the next or previous leaf pane in DFS order.
 *
 * system     Buffer system to operate on.
 * direction  Positive = next pane, negative = previous pane.
 * Returns    true if the active leaf changed.
 */
bool sol_buffer_cycle_active_pane(SolBufferSystem *system, int direction)
{
    if (!system || system->root_id == 0u || direction == 0) {
        return false;
    }

    size_t leaf_count = 0u;
    for (size_t i = 0u; i < system->node_count; ++i) {
        if (system->nodes[i].in_use && system->nodes[i].type == SOL_LAYOUT_NODE_LEAF) {
            ++leaf_count;
        }
    }

    if (leaf_count < 2u) {
        return false;
    }

    SolBufferNodeId *leaf_ids = (SolBufferNodeId *)calloc(leaf_count, sizeof(SolBufferNodeId));
    if (!leaf_ids) {
        return false;
    }

    size_t cursor = 0u;
    sol_collect_leaf_ids(system, system->root_id, leaf_ids, &cursor);

    bool changed = false;
    for (size_t i = 0u; i < cursor; ++i) {
        if (leaf_ids[i] != system->active_leaf_id) {
            continue;
        }

        size_t next;
        if (direction > 0) {
            next = (i + 1u) % cursor;
        } else {
            next = (i + cursor - 1u) % cursor;
        }
        system->active_leaf_id = leaf_ids[next];
        changed = true;
        break;
    }

    free(leaf_ids);
    if (changed) {
        bump_rev(system);
    }
    return changed;
}

bool sol_buffer_focus_previous_buffer(SolBufferSystem *system)
{
    if (!system) return false;

    /* The target is the buffer that was active immediately before the
       current one. Skip when there's no history yet or the historical
       buffer no longer exists. */
    const SolBufferId target = system->previous_focused_buffer_id;
    if (target == 0u) return false;
    if (!sol_buffer_find(system, target)) {
        /* Stale history pointing at a closed buffer — clear it so a
           subsequent call doesn't keep failing on the same dead id. */
        system->previous_focused_buffer_id = 0u;
        return false;
    }

    SolLayoutNode *leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return false;

    if (leaf->as.leaf.buffer_id == target) {
        /* Already showing it — no-op (matches user spec: "focus last
           used buffer or if already focused do nothing"). */
        return false;
    }

    if (!sol_leaf_add_tab(leaf, target)) return false;
    leaf->as.leaf.buffer_id = target;
    bump_rev(system);
    return true;
}

bool sol_buffer_set_active_leaf_buffer(SolBufferSystem *system, SolBufferId buffer_id)
{
    if (!system || !sol_buffer_find(system, buffer_id)) {
        return false;
    }

    SolLayoutNode *leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }

    if (!sol_leaf_add_tab(leaf, buffer_id)) return false;
    leaf->as.leaf.buffer_id = buffer_id;
    bump_rev(system);
    return true;
}

bool sol_buffer_set_active_leaf(SolBufferSystem *system, SolBufferNodeId leaf_id)
{
    if (!system) return false;
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }
    if (system->active_leaf_id == leaf_id) {
        return false;
    }
    system->active_leaf_id = leaf_id;
    bump_rev(system);
    return true;
}

bool sol_buffer_set_leaf_buffer(SolBufferSystem *system, SolBufferNodeId leaf_id, SolBufferId buffer_id)
{
    if (!system || !sol_buffer_find(system, buffer_id)) return false;
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return false;
    if (leaf->as.leaf.buffer_id == buffer_id) return false;
    if (!sol_leaf_add_tab(leaf, buffer_id)) return false;
    leaf->as.leaf.buffer_id = buffer_id;
    bump_rev(system);
    return true;
}

SolBufferId sol_buffer_leaf_buffer(const SolBufferSystem *system, SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return 0u;
    return leaf->as.leaf.buffer_id;
}

size_t sol_buffer_leaf_tab_count(const SolBufferSystem *system,
                                 SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    return (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF)
        ? 0u : leaf->as.leaf.tab_count;
}

SolBufferId sol_buffer_leaf_tab_at(const SolBufferSystem *system,
                                   SolBufferNodeId leaf_id, size_t index)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF ||
        index >= leaf->as.leaf.tab_count) return 0u;
    return leaf->as.leaf.tabs[index];
}

size_t sol_buffer_leaf_tab_view_start(const SolBufferSystem *system,
                                      SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    return (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF)
        ? 0u : leaf->as.leaf.tab_view_start;
}

bool sol_buffer_set_leaf_tab_view_start(SolBufferSystem *system,
                                        SolBufferNodeId leaf_id, size_t start)
{
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return false;
    if (start > leaf->as.leaf.tab_count) start = leaf->as.leaf.tab_count;
    if (start == leaf->as.leaf.tab_view_start) return false;
    leaf->as.leaf.tab_view_start = start;
    return true;
}

SolBufferId sol_buffer_leaf_tab_last_active(const SolBufferSystem *system,
                                            SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    return (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF)
        ? 0u : leaf->as.leaf.tab_last_active_id;
}

void sol_buffer_set_leaf_tab_last_active(SolBufferSystem *system,
                                         SolBufferNodeId leaf_id,
                                         SolBufferId buffer_id)
{
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (leaf && leaf->type == SOL_LAYOUT_NODE_LEAF)
        leaf->as.leaf.tab_last_active_id = buffer_id;
}

bool sol_buffer_close_leaf_tab(SolBufferSystem *system,
                               SolBufferNodeId leaf_id,
                               SolBufferId buffer_id)
{
    if (!system) return false;
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!sol_leaf_remove_tab(leaf, buffer_id)) return false;
    if (!sol_buffer_has_tab(system, buffer_id))
        return sol_buffer_close(system, buffer_id);
    bump_rev(system);
    return true;
}

/* ---- Per-leaf scroll ------------------------------------------------ */

int sol_buffer_leaf_scroll_top(const SolBufferSystem *system,
                                SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return 0;
    return leaf->as.leaf.scroll_top;
}

void sol_buffer_set_leaf_scroll_top(SolBufferSystem *system,
                                    SolBufferNodeId leaf_id, int line)
{
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return;
    if (line < 0) line = 0;
    leaf->as.leaf.scroll_top = line;
}

int sol_buffer_leaf_scroll_left(const SolBufferSystem *system,
                                 SolBufferNodeId leaf_id)
{
    const SolLayoutNode *leaf = sol_layout_find_node_const(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return 0;
    return leaf->as.leaf.scroll_left;
}

void sol_buffer_set_leaf_scroll_left(SolBufferSystem *system,
                                     SolBufferNodeId leaf_id, int col)
{
    SolLayoutNode *leaf = sol_layout_find_node(system, leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) return;
    if (col < 0) col = 0;
    leaf->as.leaf.scroll_left = col;
}

/* Recursive hit-test helper. (x,y,w,h) is the rect this subtree
   occupies in screen space. Mirrors causality's split-bar layout:
   each split places `bar_size` pixels between its two children, with
   the first child taking `ratio` of the remaining axis. */
static SolBufferNodeId sol_layout_hit_test(const SolBufferSystem *system,
                                           SolBufferNodeId node_id,
                                           float x, float y, float w, float h,
                                           float bar_size,
                                           float px, float py)
{
    const SolLayoutNode *node = sol_layout_find_node_const(system, node_id);
    if (!node) return 0u;

    if (px < x || py < y || px > x + w || py > y + h) return 0u;

    if (node->type == SOL_LAYOUT_NODE_LEAF) {
        return node->id;
    }

    const float r = node->as.split.ratio;
    if (node->as.split.direction == SOL_BUFFER_SPLIT_VERTICAL) {
        /* Vertical split = panes side-by-side (laid out horizontally). */
        const float avail = w - bar_size;
        const float first_w  = avail > 0.0f ? avail * r        : 0.0f;
        const float second_w = avail > 0.0f ? avail - first_w  : 0.0f;
        const float second_x = x + first_w + bar_size;
        if (px < second_x) {
            return sol_layout_hit_test(system, node->as.split.first_id,
                                       x, y, first_w, h, bar_size, px, py);
        }
        return sol_layout_hit_test(system, node->as.split.second_id,
                                   second_x, y, second_w, h, bar_size, px, py);
    }

    /* Horizontal split = panes stacked top-bottom (laid out vertically). */
    const float avail = h - bar_size;
    const float first_h  = avail > 0.0f ? avail * r        : 0.0f;
    const float second_h = avail > 0.0f ? avail - first_h  : 0.0f;
    const float second_y = y + first_h + bar_size;
    if (py < second_y) {
        return sol_layout_hit_test(system, node->as.split.first_id,
                                   x, y, w, first_h, bar_size, px, py);
    }
    return sol_layout_hit_test(system, node->as.split.second_id,
                               x, second_y, w, second_h, bar_size, px, py);
}

static bool sol_layout_leaf_geometry(
    const SolBufferSystem *system,
    SolBufferNodeId node_id,
    float x,
    float y,
    float w,
    float h,
    float bar_size,
    SolBufferNodeId target_leaf_id,
    SolBufferRect *out_rect)
{
    const SolLayoutNode *node = sol_layout_find_node_const(system, node_id);
    if (!node) return false;

    if (node->type == SOL_LAYOUT_NODE_LEAF) {
        if (node->id != target_leaf_id) return false;
        if (out_rect) {
            out_rect->x = x;
            out_rect->y = y;
            out_rect->w = w;
            out_rect->h = h;
        }
        return true;
    }

    const float r = node->as.split.ratio;
    if (node->as.split.direction == SOL_BUFFER_SPLIT_VERTICAL) {
        const float avail = w - bar_size;
        const float first_w  = avail > 0.0f ? avail * r       : 0.0f;
        const float second_w = avail > 0.0f ? avail - first_w : 0.0f;
        const float second_x = x + first_w + bar_size;
        if (sol_layout_leaf_geometry(system, node->as.split.first_id,
                                     x, y, first_w, h, bar_size,
                                     target_leaf_id, out_rect)) {
            return true;
        }
        return sol_layout_leaf_geometry(system, node->as.split.second_id,
                                        second_x, y, second_w, h, bar_size,
                                        target_leaf_id, out_rect);
    }

    const float avail = h - bar_size;
    const float first_h  = avail > 0.0f ? avail * r       : 0.0f;
    const float second_h = avail > 0.0f ? avail - first_h : 0.0f;
    const float second_y = y + first_h + bar_size;
    if (sol_layout_leaf_geometry(system, node->as.split.first_id,
                                 x, y, w, first_h, bar_size,
                                 target_leaf_id, out_rect)) {
        return true;
    }
    return sol_layout_leaf_geometry(system, node->as.split.second_id,
                                    x, second_y, w, second_h, bar_size,
                                    target_leaf_id, out_rect);
}

/*
 * Compute the screen rectangle occupied by a specific leaf node.
 *
 * system    Buffer system owning the layout.
 * leaf_id   Node id of the leaf whose geometry is requested.
 * root_rect Total area available to the root node.
 * bar_size  Width/height of split divider bars, in pixels.
 * out_rect  Receives the computed rectangle; may be NULL for a presence check.
 * Returns   true if the leaf was found and out_rect was filled.
 */
bool sol_buffer_leaf_geometry(const SolBufferSystem *system,
                              SolBufferNodeId leaf_id,
                              const SolBufferRect *root_rect,
                              float bar_size,
                              SolBufferRect *out_rect)
{
    if (!system || system->root_id == 0u || leaf_id == 0u || !root_rect) {
        return false;
    }
    return sol_layout_leaf_geometry(system, system->root_id,
                                    root_rect->x, root_rect->y,
                                    root_rect->w, root_rect->h,
                                    bar_size, leaf_id, out_rect);
}

/*
 * Hit-test a screen point against the layout tree and return the leaf it lands in.
 *
 * system    Buffer system owning the layout.
 * x, y      Top-left corner of the root rect in screen space.
 * w, h      Dimensions of the root rect.
 * bar_size  Width/height of split divider bars, in pixels.
 * px, py    Screen coordinates of the point to test.
 * Returns   Node id of the hit leaf, or 0 if no leaf was hit.
 */
SolBufferNodeId sol_buffer_leaf_at_point(const SolBufferSystem *system,
                                         float x, float y,
                                         float w, float h,
                                         float bar_size,
                                         float px, float py)
{
    if (!system || system->root_id == 0u) return 0u;
    return sol_layout_hit_test(system, system->root_id,
                               x, y, w, h, bar_size, px, py);
}

bool sol_buffer_set_split_ratio(SolBufferSystem *system, SolBufferNodeId split_node_id, float ratio)
{
    if (!system) {
        return false;
    }
    SolLayoutNode *node = sol_layout_find_node(system, split_node_id);
    if (!node || node->type != SOL_LAYOUT_NODE_SPLIT) {
        return false;
    }
    const float clamped = sol_clamp_ratio(ratio);
    if (node->as.split.ratio == clamped) {
        return true;   /* no observable change — skip the bump */
    }
    node->as.split.ratio = clamped;
    bump_rev(system);
    return true;
}

SolBufferId sol_buffer_active_buffer(const SolBufferSystem *system)
{
    if (!system || system->active_leaf_id == 0u) {
        return 0u;
    }

    const SolLayoutNode *leaf = sol_layout_find_node_const(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return 0u;
    }

    return leaf->as.leaf.buffer_id;
}

SolBufferNodeId sol_buffer_active_leaf(const SolBufferSystem *system)
{
    return system ? system->active_leaf_id : 0u;
}

size_t sol_buffer_count(const SolBufferSystem *system)
{
    if (!system) {
        return 0u;
    }

    size_t count = 0u;
    for (size_t i = 0u; i < system->buffer_count; ++i) {
        if (system->buffers[i].in_use) {
            ++count;
        }
    }

    return count;
}

/*
 * Return the buffer id at position index in registration order.
 *
 * system  Buffer system to query.
 * index   Zero-based index among live buffers.
 * Returns Id of the buffer at that index, or 0 if index is out of range.
 */
SolBufferId sol_buffer_at(const SolBufferSystem *system, size_t index)
{
    if (!system) {
        return 0u;
    }

    size_t cursor = 0u;
    for (size_t i = 0u; i < system->buffer_count; ++i) {
        if (!system->buffers[i].in_use) {
            continue;
        }
        if (cursor == index) {
            return system->buffers[i].id;
        }
        ++cursor;
    }
    return 0u;
}

/*
 * Cycle the buffer shown in the active leaf through that pane's tabs.
 *
 * system     Buffer system to operate on.
 * direction  Positive = next buffer, negative = previous buffer.
 * Returns    true if the displayed buffer changed.
 */
bool sol_buffer_cycle_active_leaf(SolBufferSystem *system, int direction)
{
    if (!system || direction == 0) {
        return false;
    }

    SolLayoutNode *leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }

    size_t total = leaf->as.leaf.tab_count;
    if (total < 2u) {
        return false;
    }

    size_t current = total;   /* sentinel = not found */
    for (size_t i = 0u; i < total; ++i) {
        if (leaf->as.leaf.tabs[i] == leaf->as.leaf.buffer_id) {
            current = i;
            break;
        }
    }

    size_t next;
    if (current == total) {
        next = 0u;
    } else if (direction > 0) {
        next = (current + 1u) % total;
    } else {
        next = (current + total - 1u) % total;
    }

    bool changed = (leaf->as.leaf.tabs[next] != leaf->as.leaf.buffer_id);
    leaf->as.leaf.buffer_id = leaf->as.leaf.tabs[next];
    if (changed) {
        bump_rev(system);
    }
    return changed;
}

/*
 * Recursively walk a layout subtree, invoking visitor callbacks for each leaf
 * and split. Rectangles are subdivided the same way the renderer does.
 *
 * system        Buffer system owning the layout.
 * current_rect  Screen rectangle allocated to this subtree.
 * visitor       Callback table; individual callbacks may be NULL.
 * node_id       Root of the subtree to visit.
 * bar_size      Width/height in pixels of the split divider bar.
 * user_data     Opaque value forwarded to every visitor callback.
 */
static void sol_buffer_visit_node(
    SolBufferSystem *system,
    const SolBufferRect *current_rect,
    const SolBufferWorkspaceVisitor *visitor,
    SolBufferNodeId node_id,
    float bar_size,
    void *user_data
)
{
    SolLayoutNode *node = sol_layout_find_node(system, node_id);
    if (!node || !visitor) {
        return;
    }

    if (node->type == SOL_LAYOUT_NODE_LEAF) {
        SolBuffer *buffer = sol_buffer_find(system, node->as.leaf.buffer_id);
        if (visitor->render_leaf) {
            visitor->render_leaf(buffer, node->id,
                                 node->id == system->active_leaf_id,
                                 current_rect, user_data);
        }
        return;
    }

    if (visitor->begin_split) {
        visitor->begin_split(node->as.split.direction, node->as.split.ratio, node->id, user_data);
    }

    const float r = node->as.split.ratio;
    if (node->as.split.direction == SOL_BUFFER_SPLIT_VERTICAL) {
        const float avail = current_rect->w - bar_size;
        const float first_w  = avail > 0.0f ? avail * r       : 0.0f;
        const float second_w = avail > 0.0f ? avail - first_w : 0.0f;
        const float second_x = current_rect->x + first_w + bar_size;
        SolBufferRect first_rect = {
            .x = current_rect->x,
            .y = current_rect->y,
            .w = first_w,
            .h = current_rect->h,
        };
        SolBufferRect second_rect = {
            .x = second_x,
            .y = current_rect->y,
            .w = second_w,
            .h = current_rect->h,
        };
        sol_buffer_visit_node(system, &first_rect, visitor, node->as.split.first_id,
                              bar_size, user_data);
        sol_buffer_visit_node(system, &second_rect, visitor, node->as.split.second_id,
                              bar_size, user_data);
    } else {
        const float avail = current_rect->h - bar_size;
        const float first_h  = avail > 0.0f ? avail * r       : 0.0f;
        const float second_h = avail > 0.0f ? avail - first_h : 0.0f;
        const float second_y = current_rect->y + first_h + bar_size;
        SolBufferRect first_rect = {
            .x = current_rect->x,
            .y = current_rect->y,
            .w = current_rect->w,
            .h = first_h,
        };
        SolBufferRect second_rect = {
            .x = current_rect->x,
            .y = second_y,
            .w = current_rect->w,
            .h = second_h,
        };
        sol_buffer_visit_node(system, &first_rect, visitor, node->as.split.first_id,
                              bar_size, user_data);
        sol_buffer_visit_node(system, &second_rect, visitor, node->as.split.second_id,
                              bar_size, user_data);
    }

    if (visitor->end_split) {
        visitor->end_split(user_data);
    }
}

/*
 * Walk the entire layout tree, invoking visitor callbacks for every leaf and split.
 *
 * system    Buffer system whose layout is traversed.
 * root_rect Total area available to the root node.
 * visitor   Callback table; individual callbacks may be NULL.
 * user_data Opaque value forwarded to every visitor callback.
 */
void sol_buffer_workspace_visit(
    SolBufferSystem *system,
    const SolBufferRect *root_rect,
    const SolBufferWorkspaceVisitor *visitor,
    void *user_data
)
{
    if (!system || !visitor || system->root_id == 0u || !root_rect) {
        return;
    }

    sol_buffer_visit_node(system, root_rect, visitor, system->root_id,
                          /* split bar size — keep in sync with workspace.c */
                          1.0f,
                          user_data);
}

/*
 * Invoke the buffer's render callback if it has one.
 *
 * buffer  Buffer to render; must be live and have a render op set.
 * args    Render arguments forwarded verbatim to the callback.
 */
void sol_buffer_render(SolBuffer *buffer, const SolBufferRenderArgs *args)
{
    if (!buffer || !buffer->in_use || !buffer->ops.render) {
        return;
    }

    buffer->ops.render(buffer, args, buffer->state);
}
