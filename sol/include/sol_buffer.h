#ifndef SOL_BUFFER_H
#define SOL_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward-declaration only — the buffer system does not depend on
 * causality directly; it just bumps a caller-owned u32 signal on every
 * mutation when one is attached. Pulls causality.h into sol_buffer.c
 * only via this typedef. */
typedef struct Ca_Signal Ca_Signal;

/* Forward-decl of the event bus — sol_buffer.h stays free of any
 * sol_event.h dependency. When a bus is attached, the buffer system
 * publishes sol.buffer.opened / closed / focused on the corresponding
 * mutations. See sol_event.h for the payload contract. */
typedef struct SolEventBus SolEventBus;

typedef struct SolBufferSystem SolBufferSystem;
typedef struct SolBuffer SolBuffer;

typedef uint64_t SolBufferId;
typedef uint64_t SolBufferNodeId;

typedef enum SolBufferKind {
    SOL_BUFFER_KIND_TEXT = 0,
    SOL_BUFFER_KIND_PLUGIN,
    SOL_BUFFER_KIND_TERMINAL,
    SOL_BUFFER_KIND_CUSTOM,
} SolBufferKind;

typedef enum SolBufferSplitDirection {
    SOL_BUFFER_SPLIT_HORIZONTAL = 0,
    SOL_BUFFER_SPLIT_VERTICAL,
} SolBufferSplitDirection;

typedef struct SolBufferRenderArgs {
    bool is_active;
    void *ui_context;
    SolBufferNodeId leaf_id;
} SolBufferRenderArgs;

typedef void (*SolBufferDestroyFn)(void *state);
typedef void (*SolBufferRenderFn)(const SolBuffer *buffer, const SolBufferRenderArgs *args, void *state);

typedef struct SolBufferOps {
    SolBufferDestroyFn destroy;
    SolBufferRenderFn render;
} SolBufferOps;

typedef struct SolBufferDesc {
    const char *name;
    SolBufferKind kind;
    void *state;
    SolBufferOps ops;
} SolBufferDesc;

typedef struct SolBufferSystemConfig {
    size_t initial_buffer_capacity;
    size_t initial_node_capacity;
} SolBufferSystemConfig;

typedef struct SolBufferWorkspaceVisitor {
    void (*begin_split)(SolBufferSplitDirection direction, float ratio, SolBufferNodeId node_id, void *user_data);
    void (*end_split)(void *user_data);
    void (*render_leaf)(SolBuffer *buffer, SolBufferNodeId leaf_id, bool is_active, void *user_data);
} SolBufferWorkspaceVisitor;

SolBufferSystemConfig sol_buffer_system_config_default(void);

SolBufferSystem *sol_buffer_system_create(const SolBufferSystemConfig *config);
void sol_buffer_system_destroy(SolBufferSystem *system);

/* Attach (or detach with NULL) a u32 revision signal that the buffer
 * system bumps via ca_signal_set_u32(sig, get+1) on every state
 * mutation: create/close, split, focus change, leaf-buffer swap,
 * split-ratio change, cycle. The signal is owned by the caller (and
 * typically by the causality instance — freed in ca_instance_destroy).
 * Builders that read this signal auto-subscribe and re-run on any
 * buffer-tree change without any explicit invalidate call. */
void sol_buffer_attach_revision_signal(SolBufferSystem *system, Ca_Signal *sig);

/* Attach an event bus so the buffer system publishes lifecycle events
 * (sol.buffer.opened / closed / focused). Pass NULL to detach. The
 * bus is borrowed; the caller retains ownership. Safe to call before
 * or after buffers have been created. */
void sol_buffer_attach_event_bus(SolBufferSystem *system, SolEventBus *bus);

/* Returns the currently-attached event bus, or NULL. Used by other
 * subsystems (e.g. SolTextBuffer) to publish on the same bus without
 * having to plumb it through every call. */
SolEventBus *sol_buffer_event_bus(SolBufferSystem *system);

SolBufferId sol_buffer_create(SolBufferSystem *system, const SolBufferDesc *desc);
bool sol_buffer_close(SolBufferSystem *system, SolBufferId buffer_id);

SolBuffer *sol_buffer_get(SolBufferSystem *system, SolBufferId buffer_id);
const SolBuffer *sol_buffer_get_const(const SolBufferSystem *system, SolBufferId buffer_id);

const char *sol_buffer_name(const SolBuffer *buffer);
SolBufferKind sol_buffer_kind(const SolBuffer *buffer);
SolBufferId sol_buffer_id(const SolBuffer *buffer);
void *sol_buffer_state(SolBuffer *buffer);
const void *sol_buffer_state_const(const SolBuffer *buffer);

/* Split the active leaf. `new_buffer_id` controls the buffer assigned
 * to the freshly-created leaf:
 *   - 0u                       -> empty leaf (no buffer; renders blank)
 *   - a live buffer id         -> that buffer is shown in the new pane
 * Pass the active leaf's current buffer id to mirror it. */
bool sol_buffer_split_active(
    SolBufferSystem *system,
    SolBufferSplitDirection direction,
    float ratio,
    SolBufferId new_buffer_id,
    SolBufferNodeId *out_new_leaf_id
);

/* Move focus to a neighbouring pane. `direction` > 0 advances to the
 * next leaf in tree order, < 0 to the previous. Wraps. */
bool sol_buffer_cycle_active_pane(SolBufferSystem *system, int direction);

/* Show the previously-focused buffer in the active leaf. No-op when
 * the current leaf already shows that buffer (matches "focus last
 * used or do nothing" UX) or when no prior buffer has been focused. */
bool sol_buffer_focus_previous_buffer(SolBufferSystem *system);
bool sol_buffer_set_active_leaf_buffer(SolBufferSystem *system, SolBufferId buffer_id);

/* Make `leaf_id` the active (focused) leaf. Used to implement
   click-to-focus on a buffer pane. Returns false when leaf_id is not a
   live leaf. */
bool sol_buffer_set_active_leaf(SolBufferSystem *system, SolBufferNodeId leaf_id);

/* Replace the buffer assigned to a specific leaf (not necessarily the
   active one). Used to implement clicking a tab inside a pane. Returns
   true when the leaf's buffer changed. */
bool sol_buffer_set_leaf_buffer(SolBufferSystem *system, SolBufferNodeId leaf_id, SolBufferId buffer_id);

/* Read the buffer currently assigned to a specific leaf. Returns 0
   when leaf_id is not a live leaf. */
SolBufferId sol_buffer_leaf_buffer(const SolBufferSystem *system, SolBufferNodeId leaf_id);

/* Hit-test the split tree against a point in screen space. The buffer
   area is the rect (x, y, w, h); `bar_size` is the gutter width
   between split children (mirror the value passed to ca_split_begin).
   Returns the leaf id whose rect contains (px, py), or 0 when the
   point is outside the area. */
SolBufferNodeId sol_buffer_leaf_at_point(const SolBufferSystem *system,
                                         float x, float y,
                                         float w, float h,
                                         float bar_size,
                                         float px, float py);

bool sol_buffer_set_split_ratio(SolBufferSystem *system, SolBufferNodeId split_node_id, float ratio);

SolBufferId sol_buffer_active_buffer(const SolBufferSystem *system);
SolBufferNodeId sol_buffer_active_leaf(const SolBufferSystem *system);
size_t sol_buffer_count(const SolBufferSystem *system);

/* Iterate live buffers in registration order. `index` is in [0, count).
   Returns 0u when index is out of range. Use together with
   sol_buffer_get / sol_buffer_name to drive UI like tab strips. */
SolBufferId sol_buffer_at(const SolBufferSystem *system, size_t index);

/* Cycle the buffer assigned to the active leaf through all live buffers.
   `direction` > 0 advances to the next buffer, < 0 to the previous.
   Returns true when the active leaf's buffer changed. */
bool sol_buffer_cycle_active_leaf(SolBufferSystem *system, int direction);

void sol_buffer_workspace_visit(
    SolBufferSystem *system,
    const SolBufferWorkspaceVisitor *visitor,
    void *user_data
);

void sol_buffer_render(SolBuffer *buffer, const SolBufferRenderArgs *args);

#endif
