// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_buffer.c — Unit tests for SolBufferSystem.
 *
 * Covers:
 *   - Create / destroy lifecycle
 *   - Create / close buffers, count, name lookup
 *   - sol_buffer_at iteration order
 *   - Active buffer tracking
 *   - Split pane tree: horizontal + vertical, ratio clamping
 *   - Cycle active pane
 *   - Cycle active leaf (buffer rotation within a pane)
 *   - Focus previous buffer
 *   - Set active leaf / set leaf buffer
 *   - leaf_at_point hit-testing
 *   - Workspace visitor traversal
 *   - Event bus integration: OPENED / CLOSED / FOCUSED
 *   - Regression: double-close, zero buffer_id, empty system ops
 *   - Performance: create/close 1000 buffers
 */

#include "test_harness.h"

#include "sol_buffer.h"
#include "sol_event.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static SolBufferSystem *make_system(void)
{
    SolBufferSystemConfig cfg = sol_buffer_system_config_default();
    return sol_buffer_system_create(&cfg);
}

static SolBufferId make_buffer(SolBufferSystem *sys, const char *name)
{
    return sol_buffer_create(sys, &(SolBufferDesc){
        .name  = name,
        .kind  = SOL_BUFFER_KIND_CUSTOM,
        .state = NULL,
        .ops   = {0},
    });
}

typedef struct VisitLog {
    int leaves;
    int splits;
    SolBufferNodeId leaf_ids[16];
    bool            leaf_active[16];
    SolBufferId     leaf_bufs[16];
} VisitLog;

static void visit_begin_split(SolBufferSplitDirection dir, float ratio,
                               SolBufferNodeId id, void *ud)
{
    (void)dir; (void)ratio; (void)id;
    VisitLog *v = (VisitLog *)ud;
    v->splits++;
}
static void visit_end_split(void *ud) { (void)ud; }
static void visit_leaf(SolBuffer *buf, SolBufferNodeId leaf_id,
                        bool active, const SolBufferRect *rect, void *ud)
{
    (void)rect;
    VisitLog *v = (VisitLog *)ud;
    int n = v->leaves;
    if (n < 16) {
        v->leaf_ids[n]    = leaf_id;
        v->leaf_active[n] = active;
        v->leaf_bufs[n]   = buf ? sol_buffer_id(buf) : 0u;
    }
    v->leaves++;
}

static SolBufferWorkspaceVisitor make_visitor(void)
{
    return (SolBufferWorkspaceVisitor){
        .begin_split = visit_begin_split,
        .end_split   = visit_end_split,
        .render_leaf = visit_leaf,
    };
}

static const SolBufferRect kTestRootRect = { 0.f, 0.f, 1000.f, 600.f };

/* Event log */
typedef struct EvLog {
    int opened, closed, focused;
    SolBufferId last_opened_id;
    SolBufferId last_closed_id;
    SolBufferId last_focused_id;
} EvLog;

static bool ev_handler(const SolEvent *e, void *ud)
{
    EvLog *l = (EvLog *)ud;
    if (!e->payload) return false;
    const SolBufferEventPayload *p = (const SolBufferEventPayload *)e->payload;
    if (strcmp(e->name, SOL_EVENT_BUFFER_OPENED)  == 0) { l->opened++;  l->last_opened_id  = p->buffer_id; }
    if (strcmp(e->name, SOL_EVENT_BUFFER_CLOSED)  == 0) { l->closed++;  l->last_closed_id  = p->buffer_id; }
    if (strcmp(e->name, SOL_EVENT_BUFFER_FOCUSED) == 0) { l->focused++; l->last_focused_id = p->buffer_id; }
    return false;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_create_destroy(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SOL_CHECK_NOT_NULL(T, sys);
    SOL_CHECK_EQ_SZ(T, sol_buffer_count(sys), 0);
    sol_buffer_system_destroy(sys);
    sol_buffer_system_destroy(NULL);   /* must not crash */
}

static void test_create_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();

    SolBufferId id = make_buffer(sys, "alpha");
    SOL_CHECK(T, id != 0u);
    SOL_CHECK_EQ_SZ(T, sol_buffer_count(sys), 1);

    SolBuffer *buf = sol_buffer_get(sys, id);
    SOL_CHECK_NOT_NULL(T, buf);
    SOL_CHECK_STR(T, sol_buffer_name(buf), "alpha");
    SOL_CHECK_EQ_INT(T, (int)sol_buffer_kind(buf), (int)SOL_BUFFER_KIND_CUSTOM);

    sol_buffer_system_destroy(sys);
}

static void test_close_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    bool ok = sol_buffer_close(sys, a);
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, sol_buffer_count(sys), 1);

    /* Closed buffer can no longer be looked up. */
    SOL_CHECK_NULL(T, sol_buffer_get(sys, a));

    /* Remaining buffer is still accessible. */
    SOL_CHECK_NOT_NULL(T, sol_buffer_get(sys, b));

    /* Double-close regression: must return false, not crash. */
    bool bad = sol_buffer_close(sys, a);
    SOL_CHECK(T, !bad);

    sol_buffer_system_destroy(sys);
}

static void test_buffer_at_order(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId ids[5];
    const char *names[] = {"first", "second", "third", "fourth", "fifth"};
    for (int i = 0; i < 5; ++i) ids[i] = make_buffer(sys, names[i]);

    SOL_CHECK_EQ_SZ(T, sol_buffer_count(sys), 5);
    for (size_t i = 0; i < 5; ++i) {
        SolBufferId got = sol_buffer_at(sys, i);
        SOL_CHECK_MSG(T, got == ids[i], "at(%zu) expected %llu got %llu",
                      i, (unsigned long long)ids[i], (unsigned long long)got);
    }

    /* Out of range returns 0. */
    SOL_CHECK_EQ_SZ(T, sol_buffer_at(sys, 5), 0);

    sol_buffer_system_destroy(sys);
}

static void test_active_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();

    /* No buffers — active is 0. */
    SOL_CHECK_EQ_SZ(T, sol_buffer_active_buffer(sys), 0);

    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    /* First buffer auto-becomes active leaf. */
    SOL_CHECK(T, sol_buffer_active_buffer(sys) == a ||
                 sol_buffer_active_buffer(sys) == b);

    bool ok = sol_buffer_set_active_leaf_buffer(sys, b);
    SOL_CHECK(T, ok);
    SOL_CHECK(T, sol_buffer_active_buffer(sys) == b);

    sol_buffer_system_destroy(sys);
}

static void test_split_vertical(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    SolBufferNodeId new_leaf = 0;
    bool ok = sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL,
                                       0.5f, b, &new_leaf);
    SOL_CHECK(T, ok);
    SOL_CHECK(T, new_leaf != 0u);

    /* Visitor should now see two leaves. */
    VisitLog vlog = {0};
    SolBufferWorkspaceVisitor vis = make_visitor();
    sol_buffer_workspace_visit(sys, &kTestRootRect, &vis, &vlog);
    SOL_CHECK_EQ_INT(T, vlog.leaves, 2);
    SOL_CHECK_EQ_INT(T, vlog.splits, 1);

    sol_buffer_system_destroy(sys);
}

static void test_split_horizontal(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");
    (void)a; (void)b;

    bool ok = sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_HORIZONTAL,
                                       0.3f, b, NULL);
    SOL_CHECK(T, ok);

    VisitLog vlog = {0};
    SolBufferWorkspaceVisitor vis = make_visitor();
    sol_buffer_workspace_visit(sys, &kTestRootRect, &vis, &vlog);
    SOL_CHECK_EQ_INT(T, vlog.leaves, 2);

    sol_buffer_system_destroy(sys);
}

static void test_split_ratio_clamping(SolTestCtx *T)
{
    /* Ratios outside [0.1, 0.9] should be clamped, not cause crash/bad state. */
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    (void)a;

    bool ok1 = sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.0f, 0, NULL);
    SOL_CHECK(T, ok1);   /* must not crash */

    sol_buffer_system_destroy(sys);
    sys = make_system();
    a = make_buffer(sys, "a");
    (void)a;

    bool ok2 = sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 1.0f, 0, NULL);
    SOL_CHECK(T, ok2);

    sol_buffer_system_destroy(sys);
}

static void test_cycle_active_pane(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, NULL);

    SolBufferNodeId before = sol_buffer_active_leaf(sys);
    bool ok = sol_buffer_cycle_active_pane(sys, +1);
    SOL_CHECK(T, ok);
    SolBufferNodeId after = sol_buffer_active_leaf(sys);
    SOL_CHECK(T, before != after);

    /* Cycle back — wraps around. */
    sol_buffer_cycle_active_pane(sys, +1);
    SOL_CHECK(T, sol_buffer_active_leaf(sys) == before);

    (void)a;
    sol_buffer_system_destroy(sys);
}

static void test_cycle_active_leaf(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");
    SolBufferId c = make_buffer(sys, "c");

    sol_buffer_set_active_leaf_buffer(sys, a);
    bool ok = sol_buffer_cycle_active_leaf(sys, +1);
    SOL_CHECK(T, ok);
    SolBufferId now = sol_buffer_active_buffer(sys);
    SOL_CHECK(T, now == b || now == c);  /* advanced from a */

    (void)a; (void)b; (void)c;
    sol_buffer_system_destroy(sys);
}

static void test_focus_previous_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    sol_buffer_set_active_leaf_buffer(sys, a);
    sol_buffer_set_active_leaf_buffer(sys, b);

    bool ok = sol_buffer_focus_previous_buffer(sys);
    SOL_CHECK(T, ok);
    SOL_CHECK(T, sol_buffer_active_buffer(sys) == a);

    sol_buffer_system_destroy(sys);
}

static void test_set_leaf_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");

    SolBufferNodeId new_leaf = 0;
    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, &new_leaf);

    /* Swap the new leaf's buffer to a. */
    bool ok = sol_buffer_set_leaf_buffer(sys, new_leaf, a);
    SOL_CHECK(T, ok);
    SOL_CHECK(T, sol_buffer_leaf_buffer(sys, new_leaf) == a);

    sol_buffer_system_destroy(sys);
}

static void test_leaf_at_point(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");
    (void)a;

    SolBufferNodeId leaf_b = 0;
    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, &leaf_b);

    /* With a 1000×600 buffer area, the split at 0.5 divides at x=500.
       A point at (200, 300) should hit the left pane. */
    SolBufferNodeId hit1 = sol_buffer_leaf_at_point(sys, 0.f, 0.f, 1000.f, 600.f,
                                                     1.0f, 200.f, 300.f);
    SOL_CHECK(T, hit1 != 0u);

    /* A point at (700, 300) should hit the right pane. */
    SolBufferNodeId hit2 = sol_buffer_leaf_at_point(sys, 0.f, 0.f, 1000.f, 600.f,
                                                     1.0f, 700.f, 300.f);
    SOL_CHECK(T, hit2 != 0u);
    SOL_CHECK(T, hit1 != hit2);   /* different panes */

    /* Out of bounds returns 0. */
    SolBufferNodeId miss = sol_buffer_leaf_at_point(sys, 0.f, 0.f, 1000.f, 600.f,
                                                     1.0f, 1500.f, 300.f);
    SOL_CHECK(T, miss == 0u);

    sol_buffer_system_destroy(sys);
}

static void test_leaf_geometry(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");
    (void)a;

    SolBufferNodeId leaf_a = sol_buffer_active_leaf(sys);
    SolBufferNodeId leaf_b = 0u;
    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, &leaf_b);

    SolBufferRect left_rect = {0};
    SolBufferRect right_rect = {0};
    SOL_CHECK(T, sol_buffer_leaf_geometry(sys, leaf_a,
                                          &kTestRootRect, 1.0f, &left_rect));
    SOL_CHECK(T, sol_buffer_leaf_geometry(sys, leaf_b,
                                          &kTestRootRect, 1.0f, &right_rect));
    SOL_CHECK_MSG(T, left_rect.w > 0.0f && right_rect.w > 0.0f,
                  "leaf widths must be positive");
    SOL_CHECK_MSG(T, left_rect.x != right_rect.x,
                  "split leaves must not share the same x origin");
    SOL_CHECK_MSG(T,
                  (left_rect.x == 0.0f && right_rect.x > 0.0f) ||
                  (right_rect.x == 0.0f && left_rect.x > 0.0f),
                  "one leaf should start at the left edge and the other should not");

    sol_buffer_system_destroy(sys);
}

static void test_workspace_visitor(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId a = make_buffer(sys, "a");
    SolBufferId b = make_buffer(sys, "b");
    SolBufferId c = make_buffer(sys, "c");

    /* Build a: [a | b]
       Then split b to get: [a | [b / c]] */
    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, NULL);
    /* Focus the right leaf containing b, then split it. */
    sol_buffer_set_active_leaf_buffer(sys, b);
    sol_buffer_split_active(sys, SOL_BUFFER_SPLIT_HORIZONTAL, 0.5f, c, NULL);

    VisitLog vlog = {0};
    SolBufferWorkspaceVisitor vis = make_visitor();
    sol_buffer_workspace_visit(sys, &kTestRootRect, &vis, &vlog);

    SOL_CHECK_EQ_INT(T, vlog.leaves, 3);
    SOL_CHECK_EQ_INT(T, vlog.splits, 2);

    sol_buffer_system_destroy(sys);
}

static void test_event_bus_integration(SolTestCtx *T)
{
    SolEventBusConfig ecfg = sol_event_bus_config_default();
    SolEventBus *bus = sol_event_bus_create(&ecfg);

    EvLog log = {0};
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED,  .handler = ev_handler, .user_data = &log,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_CLOSED,  .handler = ev_handler, .user_data = &log,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_FOCUSED, .handler = ev_handler, .user_data = &log,
    });

    SolBufferSystem *sys = make_system();
    sol_buffer_attach_event_bus(sys, bus);

    SolBufferId a = make_buffer(sys, "a");
    SOL_CHECK_EQ_INT(T, log.opened, 1);
    SOL_CHECK(T, log.last_opened_id == a);

    SolBufferId b = make_buffer(sys, "b");
    SOL_CHECK_EQ_INT(T, log.opened, 2);
    SOL_CHECK(T, log.last_opened_id == b);

    /* Focus a. */
    sol_buffer_set_active_leaf_buffer(sys, a);
    SOL_CHECK(T, log.focused >= 1);
    SOL_CHECK(T, log.last_focused_id == a);

    /* Close b. */
    sol_buffer_close(sys, b);
    SOL_CHECK_EQ_INT(T, log.closed, 1);
    SOL_CHECK(T, log.last_closed_id == b);

    sol_buffer_system_destroy(sys);
    sol_event_bus_destroy(bus);
}

static void test_null_safety(SolTestCtx *T)
{
    /* Public API null safety. */
    sol_buffer_system_destroy(NULL);
    SOL_CHECK_EQ_SZ(T, sol_buffer_count(NULL), 0);
    SOL_CHECK_EQ_SZ(T, sol_buffer_active_buffer(NULL), 0);
    SOL_CHECK_NULL(T, sol_buffer_get(NULL, 0));
    SOL_CHECK(T, !sol_buffer_close(NULL, 1));
    SOL_CHECK(T, !sol_buffer_set_active_leaf_buffer(NULL, 1));
    SOL_CHECK_EQ_SZ(T, sol_buffer_at(NULL, 0), 0);
    SOL_CHECK(T, !sol_buffer_cycle_active_pane(NULL, 1));
    SOL_CHECK(T, !sol_buffer_cycle_active_leaf(NULL, 1));
    SOL_CHECK(T, !sol_buffer_focus_previous_buffer(NULL));

    SolBufferWorkspaceVisitor vis = make_visitor();
    VisitLog vlog = {0};
    sol_buffer_workspace_visit(NULL, &kTestRootRect, &vis, &vlog);
    SOL_CHECK_EQ_INT(T, vlog.leaves, 0);
}

static void test_regression_zero_buffer_id(SolTestCtx *T)
{
    /* 0u is the sentinel "no buffer"; lookups must return NULL. */
    SolBufferSystem *sys = make_system();
    SOL_CHECK_NULL(T, sol_buffer_get(sys, 0u));
    SOL_CHECK(T, !sol_buffer_close(sys, 0u));
    SOL_CHECK_EQ_SZ(T, sol_buffer_leaf_buffer(sys, 0u), 0u);
    sol_buffer_system_destroy(sys);
}

/* ------------------------------------------------------------------ */
/* Performance benchmark                                               */
/* ------------------------------------------------------------------ */

static void bench_create_close(void *ud)
{
    SolBufferSystem *sys = (SolBufferSystem *)ud;
    SolBufferId id = make_buffer(sys, "bench");
    sol_buffer_close(sys, id);
}

static void run_benchmarks(void)
{
    SolBufferSystem *sys = make_system();
    sol_bench("buffer_create_close", 1000, bench_create_close, sys);
    sol_buffer_system_destroy(sys);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_buffer");

    SOL_RUN(s, test_create_destroy);
    SOL_RUN(s, test_create_buffer);
    SOL_RUN(s, test_close_buffer);
    SOL_RUN(s, test_buffer_at_order);
    SOL_RUN(s, test_active_buffer);
    SOL_RUN(s, test_split_vertical);
    SOL_RUN(s, test_split_horizontal);
    SOL_RUN(s, test_split_ratio_clamping);
    SOL_RUN(s, test_cycle_active_pane);
    SOL_RUN(s, test_cycle_active_leaf);
    SOL_RUN(s, test_focus_previous_buffer);
    SOL_RUN(s, test_set_leaf_buffer);
    SOL_RUN(s, test_leaf_at_point);
    SOL_RUN(s, test_leaf_geometry);
    SOL_RUN(s, test_workspace_visitor);
    SOL_RUN(s, test_event_bus_integration);
    SOL_RUN(s, test_null_safety);
    SOL_RUN(s, test_regression_zero_buffer_id);

    run_benchmarks();

    return sol_suite_report(&s);
}
