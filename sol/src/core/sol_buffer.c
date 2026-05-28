#include "sol_buffer.h"

#include <causality.h>   /* Ca_Signal, ca_signal_set_u32, ca_signal_get_u32 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
};

/* Single point of notification — every mutator that changes
   user-visible state calls this once on success. Cheap when no signal
   is attached, idiomatic causality when one is. */
static void bump_rev(SolBufferSystem *system)
{
    if (system && system->rev) {
        ca_signal_set_u32(system->rev, ca_signal_get_u32(system->rev) + 1u);
    }
}

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

// Clamps the split ratio to a reasonable range to prevent unusable splits.
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

    system->nodes[system->node_count++] = node;
    return &system->nodes[system->node_count - 1u];
}

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

static void sol_replace_buffer_in_layout(SolBufferSystem *system, SolBufferId old_id, SolBufferId new_id)
{
    for (size_t i = 0u; i < system->node_count; ++i) {
        SolLayoutNode *node = &system->nodes[i];
        if (!node->in_use || node->type != SOL_LAYOUT_NODE_LEAF) {
            continue;
        }

        if (node->as.leaf.buffer_id == old_id) {
            node->as.leaf.buffer_id = new_id;
        }
    }
}

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

SolBufferSystemConfig sol_buffer_system_config_default(void)
{
    SolBufferSystemConfig config;
    config.initial_buffer_capacity = 32u;
    config.initial_node_capacity = 32u;
    return config;
}

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

SolBufferId sol_buffer_create(SolBufferSystem *system, const SolBufferDesc *desc)
{
    if (!system || !desc) {
        return 0u;
    }

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
    }

    bump_rev(system);
    return buffer.id;
}

bool sol_buffer_close(SolBufferSystem *system, SolBufferId buffer_id)
{
    SolBuffer *buffer = sol_buffer_find(system, buffer_id);
    if (!buffer) {
        return false;
    }

    if (buffer->ops.destroy) {
        buffer->ops.destroy(buffer->state);
    }

    free(buffer->name);
    buffer->name = NULL;
    buffer->state = NULL;
    buffer->in_use = false;

    const SolBufferId fallback = sol_first_live_buffer_id(system);
    sol_replace_buffer_in_layout(system, buffer_id, fallback);

    bump_rev(system);
    return true;
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

    SolBufferId split_buffer_id = new_buffer_id;
    if (split_buffer_id == 0u) {
        split_buffer_id = active_leaf->as.leaf.buffer_id;
    }

    if (split_buffer_id != 0u && !sol_buffer_find(system, split_buffer_id)) {
        return false;
    }

    SolLayoutNode *new_leaf = sol_layout_create_leaf(system, split_buffer_id);
    if (!new_leaf) {
        return false;
    }

    SolLayoutNode *split = sol_layout_create_split(system, direction, ratio, active_leaf->id, new_leaf->id);
    if (!split) {
        new_leaf->in_use = false;
        return false;
    }

    const SolBufferNodeId parent_id = active_leaf->parent_id;
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

bool sol_buffer_focus_next_leaf(SolBufferSystem *system)
{
    if (!system || system->root_id == 0u) {
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

        const size_t next = (i + 1u) % cursor;
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

bool sol_buffer_set_active_leaf_buffer(SolBufferSystem *system, SolBufferId buffer_id)
{
    if (!system || !sol_buffer_find(system, buffer_id)) {
        return false;
    }

    SolLayoutNode *leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }

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

bool sol_buffer_cycle_active_leaf(SolBufferSystem *system, int direction)
{
    if (!system || direction == 0) {
        return false;
    }

    SolLayoutNode *leaf = sol_layout_find_node(system, system->active_leaf_id);
    if (!leaf || leaf->type != SOL_LAYOUT_NODE_LEAF) {
        return false;
    }

    /* Build a flat list of live buffer ids in registration order. */
    size_t total = sol_buffer_count(system);
    if (total < 2u) {
        return false;
    }

    SolBufferId *ids = (SolBufferId *)calloc(total, sizeof(SolBufferId));
    if (!ids) {
        return false;
    }

    size_t cursor = 0u;
    for (size_t i = 0u; i < system->buffer_count; ++i) {
        if (system->buffers[i].in_use) {
            ids[cursor++] = system->buffers[i].id;
        }
    }

    size_t current = total;   /* sentinel = not found */
    for (size_t i = 0u; i < total; ++i) {
        if (ids[i] == leaf->as.leaf.buffer_id) {
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

    bool changed = (ids[next] != leaf->as.leaf.buffer_id);
    leaf->as.leaf.buffer_id = ids[next];
    free(ids);
    if (changed) {
        bump_rev(system);
    }
    return changed;
}

static void sol_buffer_visit_node(
    SolBufferSystem *system,
    const SolBufferWorkspaceVisitor *visitor,
    SolBufferNodeId node_id,
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
            visitor->render_leaf(buffer, node->id, node->id == system->active_leaf_id, user_data);
        }
        return;
    }

    if (visitor->begin_split) {
        visitor->begin_split(node->as.split.direction, node->as.split.ratio, node->id, user_data);
    }

    sol_buffer_visit_node(system, visitor, node->as.split.first_id, user_data);
    sol_buffer_visit_node(system, visitor, node->as.split.second_id, user_data);

    if (visitor->end_split) {
        visitor->end_split(user_data);
    }
}

void sol_buffer_workspace_visit(
    SolBufferSystem *system,
    const SolBufferWorkspaceVisitor *visitor,
    void *user_data
)
{
    if (!system || !visitor || system->root_id == 0u) {
        return;
    }

    sol_buffer_visit_node(system, visitor, system->root_id, user_data);
}

void sol_buffer_render(SolBuffer *buffer, const SolBufferRenderArgs *args)
{
    if (!buffer || !buffer->in_use || !buffer->ops.render) {
        return;
    }

    buffer->ops.render(buffer, args, buffer->state);
}
