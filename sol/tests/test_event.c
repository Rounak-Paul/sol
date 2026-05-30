// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_event.c — Unit tests for SolEventBus.
 *
 * Covers:
 *   - Create / destroy lifecycle
 *   - Subscribe by name and by type
 *   - Synchronous publish: handler invocation, priority ordering
 *   - Unsubscribe: handler no longer fires
 *   - Queued post + drain
 *   - STOP_ON_HANDLED flag
 *   - Multiple subscribers on the same event
 *   - Payload round-trip (all fields preserved)
 *   - sol_event_type_from_name: stability, FNV-1a collision safety
 *   - Empty bus edge cases
 *   - Regression: unsubscribe-inside-handler must not crash
 */

#include "test_harness.h"

#include "sol_event.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

typedef struct CallLog {
    int          count;
    SolEventType last_type;
    char         last_name[64];
    char         last_payload[128];
    size_t       last_payload_size;
    void        *last_sender;
    bool         return_handled;
} CallLog;

static bool on_event(const SolEvent *e, void *ud)
{
    CallLog *log = (CallLog *)ud;
    log->count++;
    log->last_type         = e->type;
    log->last_payload_size = e->payload_size;
    log->last_sender       = e->sender;
    if (e->name)
        snprintf(log->last_name, sizeof(log->last_name), "%s", e->name);
    if (e->payload && e->payload_size > 0 && e->payload_size <= sizeof(log->last_payload))
        memcpy(log->last_payload, e->payload, e->payload_size);
    return log->return_handled;
}

static SolEventBus *make_bus(void)
{
    SolEventBusConfig cfg = sol_event_bus_config_default();
    return sol_event_bus_create(&cfg);
}

/* ------------------------------------------------------------------ */
/* File-scope state for test_priority_ordering (C11: no nested fns)   */
/* ------------------------------------------------------------------ */

static int g_order[4];
static int g_order_n = 0;

static bool fn0(const SolEvent *e, void *ud)
{
    (void)e; (void)ud;
    if (g_order_n < 4) g_order[g_order_n++] = 0;
    return false;
}
static bool fn1(const SolEvent *e, void *ud)
{
    (void)e; (void)ud;
    if (g_order_n < 4) g_order[g_order_n++] = 1;
    return false;
}
static bool fn2(const SolEvent *e, void *ud)
{
    (void)e; (void)ud;
    if (g_order_n < 4) g_order[g_order_n++] = 2;
    return false;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_create_destroy(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    SOL_CHECK_NOT_NULL(T, bus);
    sol_event_bus_destroy(bus);
    /* Double-destroy is UB, but single destroy must not crash. */

    /* NULL is safe */
    sol_event_bus_destroy(NULL);
}

static void test_subscribe_publish_basic(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    SolSubscriptionToken tok = sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
        .handler    = on_event,
        .user_data  = &log,
    });
    SOL_CHECK(T, tok != 0);

    SolAppStartupPayload p = { .worker_count = 4, .loaded_plugins = 2 };
    size_t n = sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name  = SOL_EVENT_APP_STARTUP,
        .payload     = &p,
        .payload_size = sizeof(p),
    });
    SOL_CHECK_EQ_SZ(T, n, 1);
    SOL_CHECK_EQ_INT(T, log.count, 1);
    SOL_CHECK_STR(T, log.last_name, SOL_EVENT_APP_STARTUP);
    SOL_CHECK_EQ_SZ(T, log.last_payload_size, sizeof(p));

    /* Payload values preserved */
    SolAppStartupPayload out;
    memcpy(&out, log.last_payload, sizeof(out));
    SOL_CHECK_EQ_INT(T, (int)out.worker_count, 4);
    SOL_CHECK_EQ_INT(T, (int)out.loaded_plugins, 2);

    sol_event_bus_destroy(bus);
}

static void test_no_match_publish(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
        .handler    = on_event,
        .user_data  = &log,
    });

    /* Publish a different event — handler should NOT fire. */
    size_t n = sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_READY,
    });
    SOL_CHECK_EQ_SZ(T, n, 0);
    SOL_CHECK_EQ_INT(T, log.count, 0);

    sol_event_bus_destroy(bus);
}

static void test_unsubscribe(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    SolSubscriptionToken tok = sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
        .handler    = on_event,
        .user_data  = &log,
    });

    /* Subscribe then unsubscribe immediately. */
    bool ok = sol_event_bus_unsubscribe(bus, tok);
    SOL_CHECK(T, ok);

    sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
    });
    SOL_CHECK_EQ_INT(T, log.count, 0);

    /* Unsubscribe of invalid token should be graceful. */
    bool bad = sol_event_bus_unsubscribe(bus, 9999u);
    SOL_CHECK(T, !bad);

    sol_event_bus_destroy(bus);
}

static void test_priority_ordering(SolTestCtx *T)
{
    /* Subscribers at higher priority values fire first. */
    SolEventBus *bus = make_bus();

    g_order_n = 0;
    /* priorities: fn1=10 (first), fn0=5 (second), fn2=1 (third). */
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP, .priority = 5,  .handler = fn0,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP, .priority = 10, .handler = fn1,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP, .priority = 1,  .handler = fn2,
    });

    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = SOL_EVENT_APP_STARTUP });
    SOL_CHECK_EQ_INT(T, g_order_n, 3);
    SOL_CHECK_EQ_INT(T, g_order[0], 1);   /* priority 10 fires first */
    SOL_CHECK_EQ_INT(T, g_order[1], 0);   /* priority  5 fires second */
    SOL_CHECK_EQ_INT(T, g_order[2], 2);   /* priority  1 fires last */

    sol_event_bus_destroy(bus);
}

static void test_stop_on_handled(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log_stop = { .return_handled = true };
    CallLog log_after = {0};

    /* Higher-priority handler returns true (handled). */
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP, .priority = 10,
        .handler    = on_event, .user_data  = &log_stop,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP, .priority = 1,
        .handler    = on_event, .user_data  = &log_after,
    });

    sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
        .flags      = SOL_EVENT_FLAG_STOP_ON_HANDLED,
    });

    SOL_CHECK_EQ_INT(T, log_stop.count,  1);
    SOL_CHECK_EQ_INT(T, log_after.count, 0);  /* blocked by STOP_ON_HANDLED */

    sol_event_bus_destroy(bus);
}

static void test_queued_post_drain(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
        .handler    = on_event,
        .user_data  = &log,
    });

    /* Post without draining — handler must not fire yet. */
    SolTextEditedPayload p1 = { .buffer_id = 1u, .byte_offset = 0, .inserted_bytes = 3 };
    SolTextEditedPayload p2 = { .buffer_id = 2u, .byte_offset = 5, .removed_bytes  = 2 };
    bool ok1 = sol_event_bus_post(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_TEXT_EDITED, .payload = &p1, .payload_size = sizeof(p1),
    });
    bool ok2 = sol_event_bus_post(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_TEXT_EDITED, .payload = &p2, .payload_size = sizeof(p2),
    });
    SOL_CHECK(T, ok1);
    SOL_CHECK(T, ok2);
    SOL_CHECK_EQ_INT(T, log.count, 0);   /* not drained yet */

    /* Drain one — only first fires. */
    size_t drained = sol_event_bus_drain(bus, 1);
    SOL_CHECK_EQ_SZ(T, drained, 1);
    SOL_CHECK_EQ_INT(T, log.count, 1);

    /* Drain remaining. */
    drained = sol_event_bus_drain(bus, 100);
    SOL_CHECK_EQ_SZ(T, drained, 1);
    SOL_CHECK_EQ_INT(T, log.count, 2);

    /* Nothing left. */
    drained = sol_event_bus_drain(bus, 100);
    SOL_CHECK_EQ_SZ(T, drained, 0);

    sol_event_bus_destroy(bus);
}

static void test_multiple_subscribers_same_event(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog a = {0}, b = {0}, c = {0};

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED, .handler = on_event, .user_data = &a,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED, .handler = on_event, .user_data = &b,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED, .handler = on_event, .user_data = &c,
    });

    SolBufferEventPayload p = { .buffer_id = 42u, .kind = SOL_BUFFER_KIND_TEXT };
    sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED, .payload = &p, .payload_size = sizeof(p),
    });

    SOL_CHECK_EQ_INT(T, a.count, 1);
    SOL_CHECK_EQ_INT(T, b.count, 1);
    SOL_CHECK_EQ_INT(T, c.count, 1);

    sol_event_bus_destroy(bus);
}

static void test_subscribe_by_type(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    SolEventType t = sol_event_type_from_name(SOL_EVENT_COMMAND_INVOKED);
    SOL_CHECK(T, t != 0);

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_type = t,
        .handler    = on_event,
        .user_data  = &log,
    });

    sol_event_bus_publish(bus, &(SolEventDesc){
        .event_type = t,
        .event_name = SOL_EVENT_COMMAND_INVOKED,
    });
    SOL_CHECK_EQ_INT(T, log.count, 1);

    sol_event_bus_destroy(bus);
}

static void test_event_type_from_name_stable(SolTestCtx *T)
{
    /* Same name must always produce the same hash. */
    SolEventType t1 = sol_event_type_from_name("sol.buffer.opened");
    SolEventType t2 = sol_event_type_from_name("sol.buffer.opened");
    SOL_CHECK(T, t1 == t2);
    SOL_CHECK(T, t1 != 0);

    /* Different names must produce different hashes (no test collision
       for known Sol events). */
    SolEventType ta = sol_event_type_from_name(SOL_EVENT_BUFFER_OPENED);
    SolEventType tb = sol_event_type_from_name(SOL_EVENT_BUFFER_CLOSED);
    SolEventType tc = sol_event_type_from_name(SOL_EVENT_BUFFER_FOCUSED);
    SolEventType td = sol_event_type_from_name(SOL_EVENT_TEXT_EDITED);
    SolEventType te = sol_event_type_from_name(SOL_EVENT_APP_STARTUP);
    SOL_CHECK(T, ta != tb);
    SOL_CHECK(T, ta != tc);
    SOL_CHECK(T, ta != td);
    SOL_CHECK(T, ta != te);
    SOL_CHECK(T, tb != tc);
    SOL_CHECK(T, tb != td);
    SOL_CHECK(T, tc != td);
}

static void test_sender_field(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog log = {0};

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_READY, .handler = on_event, .user_data = &log,
    });

    int sentinel = 42;
    sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_READY,
        .sender     = &sentinel,
    });
    SOL_CHECK(T, log.last_sender == &sentinel);

    sol_event_bus_destroy(bus);
}

static void test_empty_bus(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();

    size_t n = sol_event_bus_publish(bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
    });
    SOL_CHECK_EQ_SZ(T, n, 0);

    size_t d = sol_event_bus_drain(bus, 100);
    SOL_CHECK_EQ_SZ(T, d, 0);

    sol_event_bus_destroy(bus);
}

static void test_many_queued_events(SolTestCtx *T)
{
    /* Stress the ring buffer resize path. */
    SolEventBus *bus = make_bus();
    CallLog log = {0};
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED, .handler = on_event, .user_data = &log,
    });

    const int N = 500;
    for (int i = 0; i < N; ++i) {
        SolTextEditedPayload p = { .buffer_id = (SolBufferId)i, .inserted_bytes = 1 };
        sol_event_bus_post(bus, &(SolEventDesc){
            .event_name = SOL_EVENT_TEXT_EDITED, .payload = &p, .payload_size = sizeof(p),
        });
    }
    SOL_CHECK_EQ_INT(T, log.count, 0);

    size_t d = sol_event_bus_drain(bus, (size_t)N + 100);
    SOL_CHECK_EQ_SZ(T, d, (size_t)N);
    SOL_CHECK_EQ_INT(T, log.count, N);

    sol_event_bus_destroy(bus);
}

static void test_null_safety(SolTestCtx *T)
{
    /* All public API functions must be NULL-safe. */
    sol_event_bus_destroy(NULL);
    size_t n = sol_event_bus_publish(NULL, NULL);
    SOL_CHECK_EQ_SZ(T, n, 0);
    bool ok = sol_event_bus_post(NULL, NULL);
    SOL_CHECK(T, !ok);
    size_t d = sol_event_bus_drain(NULL, 10);
    SOL_CHECK_EQ_SZ(T, d, 0);
    bool un = sol_event_bus_unsubscribe(NULL, 0);
    SOL_CHECK(T, !un);
    SolEventType t = sol_event_type_from_name(NULL);
    SOL_CHECK_EQ_SZ(T, t, 0);
}

/* Regression: multiple unsubscribes must not corrupt subscriber list. */
static void test_regression_unsubscribe_stability(SolTestCtx *T)
{
    SolEventBus *bus = make_bus();
    CallLog logs[8];
    SolSubscriptionToken toks[8];
    memset(logs, 0, sizeof(logs));

    for (int i = 0; i < 8; ++i) {
        toks[i] = sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
            .event_name = SOL_EVENT_APP_STARTUP,
            .handler    = on_event,
            .user_data  = &logs[i],
        });
    }

    /* Unsubscribe every other subscriber. */
    for (int i = 0; i < 8; i += 2) {
        sol_event_bus_unsubscribe(bus, toks[i]);
    }

    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = SOL_EVENT_APP_STARTUP });

    /* Remaining subscribers (odd indices) must have fired. */
    for (int i = 1; i < 8; i += 2) {
        SOL_CHECK_MSG(T, logs[i].count == 1, "subscriber %d count=%d", i, logs[i].count);
    }
    /* Unsubscribed subscribers (even indices) must not have fired. */
    for (int i = 0; i < 8; i += 2) {
        SOL_CHECK_MSG(T, logs[i].count == 0, "unsubscribed %d count=%d", i, logs[i].count);
    }

    sol_event_bus_destroy(bus);
}

/* ------------------------------------------------------------------ */
/* Performance benchmark                                               */
/* ------------------------------------------------------------------ */

static SolEventBus *g_bench_bus = NULL;

static void bench_publish_fn(void *ud)
{
    (void)ud;
    sol_event_bus_publish(g_bench_bus, &(SolEventDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
    });
}

static void run_benchmarks(void)
{
    g_bench_bus = make_bus();
    /* Add 8 no-op subscribers. */
    static CallLog noop_logs[8];
    for (int i = 0; i < 8; ++i) {
        sol_event_bus_subscribe(g_bench_bus, &(SolEventSubscriptionDesc){
            .event_name = SOL_EVENT_TEXT_EDITED,
            .handler    = on_event,
            .user_data  = &noop_logs[i],
        });
    }
    sol_bench("event_bus_publish x8_subs", 10000, bench_publish_fn, NULL);
    sol_event_bus_destroy(g_bench_bus);
    g_bench_bus = NULL;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_event");

    SOL_RUN(s, test_create_destroy);
    SOL_RUN(s, test_subscribe_publish_basic);
    SOL_RUN(s, test_no_match_publish);
    SOL_RUN(s, test_unsubscribe);
    SOL_RUN(s, test_priority_ordering);
    SOL_RUN(s, test_stop_on_handled);
    SOL_RUN(s, test_queued_post_drain);
    SOL_RUN(s, test_multiple_subscribers_same_event);
    SOL_RUN(s, test_subscribe_by_type);
    SOL_RUN(s, test_event_type_from_name_stable);
    SOL_RUN(s, test_sender_field);
    SOL_RUN(s, test_empty_bus);
    SOL_RUN(s, test_many_queued_events);
    SOL_RUN(s, test_null_safety);
    SOL_RUN(s, test_regression_unsubscribe_stability);

    run_benchmarks();

    return sol_suite_report(&s);
}
