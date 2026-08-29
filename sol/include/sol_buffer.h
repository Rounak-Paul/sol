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

/* Discriminates the content model of a SolBuffer. */
typedef enum SolBufferKind {
    SOL_BUFFER_KIND_TEXT = 0,
    SOL_BUFFER_KIND_PLUGIN,
    SOL_BUFFER_KIND_TERMINAL,
    SOL_BUFFER_KIND_CUSTOM,
} SolBufferKind;

/* Axis along which a leaf is split into two children. */
typedef enum SolBufferSplitDirection {
    SOL_BUFFER_SPLIT_HORIZONTAL = 0,
    SOL_BUFFER_SPLIT_VERTICAL,
} SolBufferSplitDirection;

/* Rectangle in workspace layout pixels. */
typedef struct SolBufferRect {
    float x;
    float y;
    float w;
    float h;
} SolBufferRect;

/* Geometry for a rendered buffer leaf. `rect` is in the same layout
 * space used by the workspace split tree and excludes its pane-local tab
 * strip. */
typedef struct SolBufferRenderArgs {
    bool is_active;
    void *ui_context;
    SolBufferNodeId leaf_id;
    SolBufferRect rect;
    /* Owning buffer system — available so renderers can access per-leaf
     * state (e.g. independent scroll positions) without a separate context
     * pointer. May be NULL in tests that do not provide a system. */
    SolBufferSystem *system;
} SolBufferRenderArgs;

/* Callback invoked to release the caller-owned state attached to a buffer. */
typedef void (*SolBufferDestroyFn)(void *state);

/*
 * Callback invoked by the buffer system during each render pass for a leaf.
 *
 * buffer  The buffer being rendered.
 * args    Geometry and context for the current frame.
 * state   Caller-owned state pointer registered with the buffer.
 */
typedef void (*SolBufferRenderFn)(const SolBuffer *buffer, const SolBufferRenderArgs *args, void *state);

/* Vtable of lifecycle callbacks supplied by each buffer's owner. */
typedef struct SolBufferOps {
    SolBufferDestroyFn destroy;
    SolBufferRenderFn render;
} SolBufferOps;

/* Initialization descriptor passed to sol_buffer_create. */
typedef struct SolBufferDesc {
    const char *name;
    SolBufferKind kind;
    void *state;
    SolBufferOps ops;
} SolBufferDesc;

/* Tuning knobs for the buffer system's internal storage. */
typedef struct SolBufferSystemConfig {
    size_t initial_buffer_capacity;
    size_t initial_node_capacity;
} SolBufferSystemConfig;

/*
 * Visitor interface for walking the buffer split tree.
 *
 * begin_split   Called when entering an interior split node.
 * end_split     Called when leaving an interior split node.
 * render_leaf   Called for each visible leaf with its buffer and geometry.
 */
typedef struct SolBufferWorkspaceVisitor {
    void (*begin_split)(SolBufferSplitDirection direction,
                        float ratio,
                        SolBufferNodeId node_id,
                        void *user_data);
    void (*end_split)(void *user_data);
    void (*render_leaf)(SolBuffer *buffer,
                        SolBufferNodeId leaf_id,
                        bool is_active,
                        const SolBufferRect *rect,
                        void *user_data);
} SolBufferWorkspaceVisitor;

/* Return a SolBufferSystemConfig populated with sensible defaults. */
SolBufferSystemConfig sol_buffer_system_config_default(void);

/*
 * Create a new buffer system.
 *
 * config  Capacity hints; pass the result of sol_buffer_system_config_default()
 *         for reasonable defaults.
 * Returns A heap-allocated system, or NULL on allocation failure.
 */
SolBufferSystem *sol_buffer_system_create(const SolBufferSystemConfig *config);

/* Destroy the buffer system and free all internal resources. */
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

/*
 * Register a new buffer and return its id.
 *
 * system  The buffer system that will own the buffer.
 * desc    Name, kind, state, and ops for the new buffer.
 * Returns A non-zero id on success, or 0 on failure.
 */
SolBufferId sol_buffer_create(SolBufferSystem *system, const SolBufferDesc *desc);

/*
 * Remove a buffer from the system and invoke its destroy callback.
 *
 * system     The owning buffer system.
 * buffer_id  Id of the buffer to remove.
 * Returns    true if the buffer was found and closed.
 */
bool sol_buffer_close(SolBufferSystem *system, SolBufferId buffer_id);

/* Close every live buffer. Returns the number successfully closed. */
size_t sol_buffer_close_all(SolBufferSystem *system);

/* Close one pane's tab, destroying the buffer only when no pane retains it. */
bool sol_buffer_close_leaf_tab(SolBufferSystem *system,
                               SolBufferNodeId leaf_id,
                               SolBufferId buffer_id);

/*
 * Look up a mutable buffer by id.
 *
 * system     The owning buffer system.
 * buffer_id  Id to look up.
 * Returns    Pointer to the buffer, or NULL when not found.
 */
SolBuffer *sol_buffer_get(SolBufferSystem *system, SolBufferId buffer_id);

/*
 * Look up a read-only buffer by id.
 *
 * system     The owning buffer system.
 * buffer_id  Id to look up.
 * Returns    Const pointer to the buffer, or NULL when not found.
 */
const SolBuffer *sol_buffer_get_const(const SolBufferSystem *system, SolBufferId buffer_id);

/* Returns the display name registered with the buffer. */
const char *sol_buffer_name(const SolBuffer *buffer);

/* Returns the content-model kind of the buffer. */
SolBufferKind sol_buffer_kind(const SolBuffer *buffer);

/* Returns the unique id of the buffer. */
SolBufferId sol_buffer_id(const SolBuffer *buffer);

/* Returns the mutable caller-owned state pointer stored in the buffer. */
void *sol_buffer_state(SolBuffer *buffer);

/* Returns the read-only caller-owned state pointer stored in the buffer. */
const void *sol_buffer_state_const(const SolBuffer *buffer);

/*
 * Split the active leaf, creating a new sibling pane.
 *
 * system          The buffer system.
 * direction       Whether to split horizontally or vertically.
 * ratio           Fraction [0,1] allocated to the first (existing) child.
 * new_buffer_id   Buffer to show in the new leaf: 0 for an empty leaf,
 *                 or any live buffer id. Pass the active leaf's current id
 *                 to mirror the same buffer.
 * out_new_leaf_id If non-NULL, receives the node id of the new leaf.
 * Returns         true on success.
 */
bool sol_buffer_split_active(
    SolBufferSystem *system,
    SolBufferSplitDirection direction,
    float ratio,
    SolBufferId new_buffer_id,
    SolBufferNodeId *out_new_leaf_id
);

/*
 * Move focus to a neighbouring pane.
 *
 * system     The buffer system.
 * direction  Positive to advance to the next leaf in tree order,
 *            negative to go to the previous. Wraps around.
 * Returns    true on success.
 */
bool sol_buffer_cycle_active_pane(SolBufferSystem *system, int direction);

/*
 * Show the previously-focused buffer in the active leaf.
 *
 * No-op when the active leaf already shows that buffer, or when no
 * prior buffer has been focused.
 *
 * system   The buffer system.
 * Returns  true if the active leaf's buffer changed.
 */
bool sol_buffer_focus_previous_buffer(SolBufferSystem *system);

/*
 * Assign a buffer to the active leaf and focus it.
 *
 * system     The buffer system.
 * buffer_id  Id of the buffer to show.
 * Returns    true if the assignment succeeded.
 */
bool sol_buffer_set_active_leaf_buffer(SolBufferSystem *system, SolBufferId buffer_id);

/*
 * Make a specific leaf the active (focused) leaf.
 *
 * system   The buffer system.
 * leaf_id  Id of the leaf to focus.
 * Returns  false when leaf_id is not a live leaf.
 */
bool sol_buffer_set_active_leaf(SolBufferSystem *system, SolBufferNodeId leaf_id);

/*
 * Replace the buffer assigned to any leaf, not necessarily the active one.
 *
 * system     The buffer system.
 * leaf_id    The leaf whose buffer will be replaced.
 * buffer_id  The buffer to show in that leaf.
 * Returns    true when the leaf's buffer changed.
 */
bool sol_buffer_set_leaf_buffer(SolBufferSystem *system, SolBufferNodeId leaf_id, SolBufferId buffer_id);

/*
 * Return the buffer currently assigned to a specific leaf.
 *
 * system   The buffer system.
 * leaf_id  The leaf to query.
 * Returns  The buffer id, or 0 when leaf_id is not a live leaf.
 */
SolBufferId sol_buffer_leaf_buffer(const SolBufferSystem *system, SolBufferNodeId leaf_id);

/* Pane-local tabs and their horizontal viewport. */
size_t sol_buffer_leaf_tab_count(const SolBufferSystem *system,
                                 SolBufferNodeId leaf_id);
SolBufferId sol_buffer_leaf_tab_at(const SolBufferSystem *system,
                                   SolBufferNodeId leaf_id, size_t index);
size_t sol_buffer_leaf_tab_view_start(const SolBufferSystem *system,
                                      SolBufferNodeId leaf_id);
bool sol_buffer_set_leaf_tab_view_start(SolBufferSystem *system,
                                        SolBufferNodeId leaf_id, size_t start);
SolBufferId sol_buffer_leaf_tab_last_active(const SolBufferSystem *system,
                                            SolBufferNodeId leaf_id);
void sol_buffer_set_leaf_tab_last_active(SolBufferSystem *system,
                                         SolBufferNodeId leaf_id,
                                         SolBufferId buffer_id);

/* Per-leaf independent scroll positions.  When the same buffer is open
 * in multiple panes, each leaf tracks its own viewport so panes can show
 * different parts of the same file simultaneously. */

/* Return the first visible line for a leaf's viewport (0 when not found). */
int  sol_buffer_leaf_scroll_top (const SolBufferSystem *system,
                                 SolBufferNodeId leaf_id);

/* Set the first visible line for a leaf's viewport. */
void sol_buffer_set_leaf_scroll_top (SolBufferSystem *system,
                                     SolBufferNodeId leaf_id, int line);

/* Return the first visible visual column for a leaf's viewport (0 when not found). */
int  sol_buffer_leaf_scroll_left(const SolBufferSystem *system,
                                 SolBufferNodeId leaf_id);

/* Set the first visible visual column for a leaf's viewport. */
void sol_buffer_set_leaf_scroll_left(SolBufferSystem *system,
                                     SolBufferNodeId leaf_id, int col);

/*
 * Hit-test the split tree against a point in screen space.
 *
 * system    The buffer system.
 * x, y      Origin of the buffer area rectangle.
 * w, h      Dimensions of the buffer area rectangle.
 * bar_size  Gutter width between split children.
 * px, py    The point to test.
 * Returns   The leaf id whose rect contains (px, py), or 0 when the
 *           point is outside the area.
 */
SolBufferNodeId sol_buffer_leaf_at_point(const SolBufferSystem *system,
                                         float x, float y,
                                         float w, float h,
                                         float bar_size,
                                         float px, float py);

/*
 * Update the split ratio of an interior split node.
 *
 * system         The buffer system.
 * split_node_id  Id of the split node to update.
 * ratio          New fraction [0,1] for the first child.
 * Returns        true on success.
 */
bool sol_buffer_set_split_ratio(SolBufferSystem *system, SolBufferNodeId split_node_id, float ratio);
/*
 * Resolve the screen-space rectangle for a specific leaf.
 *
 * system     The buffer system.
 * leaf_id    The leaf whose geometry to resolve.
 * root_rect  Workspace rectangle that forms the root of the layout.
 * bar_size   Gutter width between split children.
 * out_rect   Receives the computed rectangle on success.
 * Returns    false when the leaf is not present.
 */
bool sol_buffer_leaf_geometry(const SolBufferSystem *system,
                              SolBufferNodeId leaf_id,
                              const SolBufferRect *root_rect,
                              float bar_size,
                              SolBufferRect *out_rect);

/* Returns the id of the buffer shown in the currently focused leaf (0 if none). */
SolBufferId sol_buffer_active_buffer(const SolBufferSystem *system);

/* Returns the node id of the currently focused leaf. */
SolBufferNodeId sol_buffer_active_leaf(const SolBufferSystem *system);

/* Returns the total number of live buffers registered in the system. */
size_t sol_buffer_count(const SolBufferSystem *system);

/*
 * Return the buffer id at a given position in registration order.
 *
 * system  The buffer system.
 * index   Zero-based index in [0, sol_buffer_count()).
 * Returns The buffer id, or 0 when index is out of range.
 */
SolBufferId sol_buffer_at(const SolBufferSystem *system, size_t index);

/*
 * Cycle the buffer shown in the active leaf through that pane's tabs.
 *
 * system     The buffer system.
 * direction  Positive to advance to the next buffer, negative for previous.
 * Returns    true when the active leaf's buffer changed.
 */
bool sol_buffer_cycle_active_leaf(SolBufferSystem *system, int direction);

/*
 * Walk the entire split tree, calling visitor callbacks for each node.
 *
 * system     The buffer system.
 * root_rect  Workspace rectangle used to compute leaf geometry.
 * bar_size   Divider thickness in the same pixel space as root_rect.
 * visitor    Callback table for split and leaf events.
 * user_data  Passed unchanged to every visitor callback.
 */
void sol_buffer_workspace_visit(
    SolBufferSystem *system,
    const SolBufferRect *root_rect,
    float bar_size,
    const SolBufferWorkspaceVisitor *visitor,
    void *user_data
);

/*
 * Invoke a buffer's render callback for a single frame.
 *
 * buffer  The buffer to render.
 * args    Geometry and context for the current frame.
 */
void sol_buffer_render(SolBuffer *buffer, const SolBufferRenderArgs *args);

#endif
