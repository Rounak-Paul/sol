// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_integration.c — End-to-end integration and flow tests for Sol.
 *
 * These tests exercise multiple subsystems together to verify that the
 * plumbing between them is correct end-to-end.
 *
 * Scenarios covered:
 *   1. Buffer lifecycle events: open → focus → close → no events after close
 *   2. Multiple buffers with focus history: BUFFER_FOCUSED payloads carry
 *      correct IDs through open → focus switches → focus previous.
 *   3. Text edit events: insert/backspace correctly update rope via event bus.
 *   4. Event type name roundtrip: type_from_name(name_of_type(T)) == T.
 *   5. Buffer split + cycle + leaf_at_point: spatial and logical navigation.
 *   6. Queued events drain across subsystems.
 *   7. Regression: close-active-buffer leaves system consistent
 *      (active buffer is reassigned or 0, count decremented).
 *   8. Regression: subscribe after publish — handler does NOT retroactively
 *      fire for past events.
 */

#include "test_harness.h"

#include "sol_buffer.h"
#include "sol_text_buffer.h"
#include "sol_event.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* File-scope event handlers (C11: nested functions are not allowed)   */
/* ------------------------------------------------------------------ */

typedef struct EditLog {
    int count;
    SolTextEditedPayload edits[64];
} EditLog;

typedef struct U8Log { int count; size_t total_inserted; } U8Log;

static bool edit_handler(const SolEvent *e, void *ud)
{
    EditLog *l = (EditLog *)ud;
    if (e->payload && e->payload_size >= sizeof(SolTextEditedPayload)
        && l->count < 64) {
        memcpy(&l->edits[l->count++], e->payload, sizeof(SolTextEditedPayload));
    }
    return false;
}

static bool u8_handler(const SolEvent *e, void *ud)
{
    U8Log *l = (U8Log *)ud;
    if (e->payload && e->payload_size >= sizeof(SolTextEditedPayload)) {
        const SolTextEditedPayload *p = (const SolTextEditedPayload *)e->payload;
        l->total_inserted += p->inserted_bytes;
    }
    l->count++;
    return false;
}

/* ------------------------------------------------------------------ */
/* Shared infrastructure                                               */
/* ------------------------------------------------------------------ */

typedef struct IntegrationCtx {
    SolEventBus    *bus;
    SolBufferSystem *sys;
} IntegrationCtx;

static IntegrationCtx make_ctx(void)
{
    SolEventBusConfig ecfg = sol_event_bus_config_default();
    SolBufferSystemConfig bcfg = sol_buffer_system_config_default();
    IntegrationCtx ctx;
    ctx.bus = sol_event_bus_create(&ecfg);
    ctx.sys = sol_buffer_system_create(&bcfg);
    sol_buffer_attach_event_bus(ctx.sys, ctx.bus);
    return ctx;
}

static void free_ctx(IntegrationCtx *ctx)
{
    sol_buffer_system_destroy(ctx->sys);
    sol_event_bus_destroy(ctx->bus);
}

static SolBufferId make_tb(IntegrationCtx *ctx, const char *name)
{
    return sol_text_buffer_open_empty(ctx->sys, name, NULL);
}

/* Generic event recorder. */
typedef struct EvRecord {
    int       count;
    char      names[32][64];    /* last 32 event names */
    SolBufferId buf_ids[32];
} EvRecord;

static bool ev_record(const SolEvent *e, void *ud)
{
    EvRecord *r = (EvRecord *)ud;
    int i = r->count % 32;
    snprintf(r->names[i], sizeof(r->names[i]), "%s", e->name ? e->name : "");
    if (e->payload && e->payload_size >= sizeof(SolBufferEventPayload)) {
        const SolBufferEventPayload *p = (const SolBufferEventPayload *)e->payload;
        r->buf_ids[i] = p->buffer_id;
    } else {
        r->buf_ids[i] = 0u;
    }
    r->count++;
    return false;
}

static void subscribe_all_buffer_events(SolEventBus *bus, EvRecord *rec)
{
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_OPENED,  .handler = ev_record, .user_data = rec,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_CLOSED,  .handler = ev_record, .user_data = rec,
    });
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_FOCUSED, .handler = ev_record, .user_data = rec,
    });
}

/* ------------------------------------------------------------------ */
/* Scenario 1: Buffer lifecycle events                                 */
/* ------------------------------------------------------------------ */

static void test_lifecycle_events(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();
    EvRecord rec = {0};
    subscribe_all_buffer_events(ctx.bus, &rec);

    /* Creating the first buffer fires OPENED then FOCUSED (auto-focuses into
       the single root leaf). */
    SolBufferId a = make_tb(&ctx, "a");
    SOL_CHECK_MSG(T, rec.count >= 2, "expected OPENED+FOCUSED events, got %d", rec.count);
    SOL_CHECK_STR(T, rec.names[0], SOL_EVENT_BUFFER_OPENED);
    SOL_CHECK_STR(T, rec.names[1], SOL_EVENT_BUFFER_FOCUSED);

    /* Creating a second buffer fires OPENED; the active leaf is still
       showing `a`, so no FOCUSED is emitted. */
    int before_b = rec.count;
    SolBufferId b = make_tb(&ctx, "b");
    SOL_CHECK(T, rec.count > before_b);
    SOL_CHECK_STR(T, rec.names[before_b % 32], SOL_EVENT_BUFFER_OPENED);

    /* Explicitly switching focus fires FOCUSED. */
    int before_focus = rec.count;
    sol_buffer_set_active_leaf_buffer(ctx.sys, b);
    SOL_CHECK(T, rec.count > before_focus);
    SOL_CHECK_STR(T, rec.names[(rec.count - 1) % 32], SOL_EVENT_BUFFER_FOCUSED);

    /* Close fires CLOSED. */
    int before_close = rec.count;
    sol_buffer_close(ctx.sys, a);
    SOL_CHECK(T, rec.count > before_close);
    SOL_CHECK_STR(T, rec.names[(rec.count - 1) % 32], SOL_EVENT_BUFFER_CLOSED);
    SOL_CHECK(T, rec.buf_ids[(rec.count - 1) % 32] == a);

    /* After close, further publishes don't mention the closed buffer. */
    int after_close = rec.count;
    SolBufferId c = make_tb(&ctx, "c");
    SOL_CHECK(T, rec.buf_ids[(rec.count - 1) % 32] == c);
    SOL_CHECK(T, rec.count > after_close);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: Focus history with multiple buffers                    */
/* ------------------------------------------------------------------ */

static void test_focus_history(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();
    EvRecord rec = {0};
    sol_event_bus_subscribe(ctx.bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_BUFFER_FOCUSED, .handler = ev_record, .user_data = &rec,
    });

    SolBufferId a = make_tb(&ctx, "a");
    SolBufferId b = make_tb(&ctx, "b");
    SolBufferId c = make_tb(&ctx, "c");

    rec.count = 0;  /* Reset after the auto-focus from open. */

    sol_buffer_set_active_leaf_buffer(ctx.sys, a);
    sol_buffer_set_active_leaf_buffer(ctx.sys, b);
    sol_buffer_set_active_leaf_buffer(ctx.sys, c);

    /* Now focus_previous should go back to b (immediately prior to c). */
    bool ok = sol_buffer_focus_previous_buffer(ctx.sys);
    SOL_CHECK(T, ok);
    SOL_CHECK(T, sol_buffer_active_buffer(ctx.sys) == b);

    /* Focusing b updates the history: prev becomes c (what was active before b
       in this session). The API is a 2-entry toggle, not a stack — calling
       focus_previous again returns to c, not all the way back to a. */
    sol_buffer_focus_previous_buffer(ctx.sys);
    SOL_CHECK(T, sol_buffer_active_buffer(ctx.sys) == c);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: Text edit events from insert / backspace               */
/* ------------------------------------------------------------------ */

static void test_text_edit_events(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();

    EditLog elog = {0};

    sol_event_bus_subscribe(ctx.bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
        .handler    = edit_handler,
        .user_data  = &elog,
    });

    SolBufferId id = make_tb(&ctx, "t");
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(ctx.sys, id));
    SOL_CHECK_NOT_NULL(T, tb);

    /* Insert 3 chars. */
    sol_text_buffer_insert_codepoint(tb, 'H');
    sol_text_buffer_insert_codepoint(tb, 'i');
    sol_text_buffer_insert_codepoint(tb, '!');
    SOL_CHECK_EQ_INT(T, elog.count, 3);

    /* Each edit inserted exactly 1 byte. */
    for (int i = 0; i < 3; ++i) {
        SOL_CHECK_MSG(T, elog.edits[i].inserted_bytes == 1,
                      "edit[%d].inserted_bytes=%zu", i, elog.edits[i].inserted_bytes);
        SOL_CHECK_EQ_SZ(T, elog.edits[i].removed_bytes, 0);
    }
    /* Offsets are consecutive: 0, 1, 2. */
    SOL_CHECK_EQ_SZ(T, elog.edits[0].byte_offset, 0);
    SOL_CHECK_EQ_SZ(T, elog.edits[1].byte_offset, 1);
    SOL_CHECK_EQ_SZ(T, elog.edits[2].byte_offset, 2);

    /* Backspace removes 1 char. */
    sol_text_buffer_backspace(tb);
    SOL_CHECK_EQ_INT(T, elog.count, 4);
    SOL_CHECK_EQ_SZ(T, elog.edits[3].removed_bytes,  1);
    SOL_CHECK_EQ_SZ(T, elog.edits[3].inserted_bytes, 0);
    SOL_CHECK_EQ_SZ(T, elog.edits[3].byte_offset,    2);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: Event type name roundtrip                              */
/* ------------------------------------------------------------------ */

static void test_event_type_name_roundtrip(SolTestCtx *T)
{
    /* Compute type from name, then verify the known names match their
       types when looked up again. */
    static const char *names[] = {
        SOL_EVENT_APP_STARTUP,
        SOL_EVENT_APP_READY,
        SOL_EVENT_BUFFER_OPENED,
        SOL_EVENT_BUFFER_CLOSED,
        SOL_EVENT_BUFFER_FOCUSED,
        SOL_EVENT_TEXT_EDITED,
        SOL_EVENT_COMMAND_INVOKED,
        SOL_EVENT_FILE_TREE_ROOT,
    };
    const size_t N = sizeof(names) / sizeof(names[0]);

    SolEventType types[16];
    for (size_t i = 0; i < N; ++i) {
        types[i] = sol_event_type_from_name(names[i]);
        SOL_CHECK_MSG(T, types[i] != 0, "type_from_name('%s') returned 0", names[i]);
    }

    /* All types must be distinct. */
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 1; j < N; ++j) {
            SOL_CHECK_MSG(T, types[i] != types[j],
                          "'%s' and '%s' hash collision!", names[i], names[j]);
        }
    }

    /* Recomputing produces the same values. */
    for (size_t i = 0; i < N; ++i) {
        SOL_CHECK(T, sol_event_type_from_name(names[i]) == types[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Scenario 5: Split + cycle + leaf_at_point                          */
/* ------------------------------------------------------------------ */

static void test_split_cycle_leaf(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();

    SolBufferId a = make_tb(&ctx, "a");
    SolBufferId b = make_tb(&ctx, "b");
    SolBufferId c = make_tb(&ctx, "c");

    /* Create: [a | b] */
    sol_buffer_split_active(ctx.sys, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, b, NULL);

    /* Cycle pane: active leaf changes. */
    SolBufferNodeId before = sol_buffer_active_leaf(ctx.sys);
    sol_buffer_cycle_active_pane(ctx.sys, +1);
    SOL_CHECK(T, sol_buffer_active_leaf(ctx.sys) != before);

    /* Cycle back. */
    sol_buffer_cycle_active_pane(ctx.sys, +1);
    SOL_CHECK(T, sol_buffer_active_leaf(ctx.sys) == before);

    /* Focus the right pane (b) then split it to get [a | [b / c]]. */
    sol_buffer_set_active_leaf_buffer(ctx.sys, b);
    sol_buffer_split_active(ctx.sys, SOL_BUFFER_SPLIT_HORIZONTAL, 0.5f, c, NULL);

    /* leaf_at_point: area 0,0,1000,600.  Point (200,300) in left pane (a). */
    SolBufferNodeId left = sol_buffer_leaf_at_point(ctx.sys, 0,0,1000,600,1.f, 200, 300);
    SolBufferNodeId right_top = sol_buffer_leaf_at_point(ctx.sys, 0,0,1000,600,1.f, 700, 150);
    SolBufferNodeId right_bot = sol_buffer_leaf_at_point(ctx.sys, 0,0,1000,600,1.f, 700, 450);

    SOL_CHECK(T, left != 0u);
    SOL_CHECK(T, right_top != 0u);
    SOL_CHECK(T, right_bot != 0u);
    SOL_CHECK(T, left != right_top);
    SOL_CHECK(T, left != right_bot);
    SOL_CHECK(T, right_top != right_bot);

    /* The leaf containing a should show a's buffer. */
    SOL_CHECK(T, sol_buffer_leaf_buffer(ctx.sys, left) == a);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: Queued events drain                                     */
/* ------------------------------------------------------------------ */

static void test_queued_drain_integration(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();
    EvRecord rec = {0};
    subscribe_all_buffer_events(ctx.bus, &rec);

    /* Post (not publish) BUFFER_OPENED manually to test drain path. */
    SolBufferEventPayload p = { .buffer_id = 99u, .kind = SOL_BUFFER_KIND_TEXT };
    sol_event_bus_post(ctx.bus, &(SolEventDesc){
        .event_name   = SOL_EVENT_BUFFER_OPENED,
        .payload      = &p,
        .payload_size = sizeof(p),
    });
    SOL_CHECK_EQ_INT(T, rec.count, 0);  /* not drained yet */

    sol_event_bus_drain(ctx.bus, 100);
    SOL_CHECK(T, rec.count > 0);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: Close-active-buffer regression                         */
/* ------------------------------------------------------------------ */

static void test_regression_close_active(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();

    SolBufferId a = make_tb(&ctx, "a");
    SolBufferId b = make_tb(&ctx, "b");
    SolBufferId c = make_tb(&ctx, "c");
    (void)c;

    /* Focus b. */
    sol_buffer_set_active_leaf_buffer(ctx.sys, b);
    SOL_CHECK(T, sol_buffer_active_buffer(ctx.sys) == b);

    /* Close the active buffer. */
    sol_buffer_close(ctx.sys, b);
    SOL_CHECK_EQ_SZ(T, sol_buffer_count(ctx.sys), 2);

    /* Active must be either a, c, or 0 — but NOT b. */
    SolBufferId active = sol_buffer_active_buffer(ctx.sys);
    SOL_CHECK_MSG(T, active != b, "active buffer is the closed buffer!");

    /* The closed buffer must be gone. */
    SOL_CHECK_NULL(T, sol_buffer_get(ctx.sys, b));

    /* Remaining buffers are still accessible. */
    SOL_CHECK_NOT_NULL(T, sol_buffer_get(ctx.sys, a));

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: Subscribe-after-publish does not retroactively fire    */
/* ------------------------------------------------------------------ */

static void test_regression_late_subscribe(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();

    /* Publish before subscribing. */
    sol_event_bus_publish(ctx.bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
    });

    EvRecord rec = {0};
    sol_event_bus_subscribe(ctx.bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
        .handler    = ev_record,
        .user_data  = &rec,
    });

    SOL_CHECK_EQ_INT(T, rec.count, 0);  /* must not have fired retroactively */

    /* Subsequent publish DOES fire. */
    sol_event_bus_publish(ctx.bus, &(SolEventDesc){
        .event_name = SOL_EVENT_APP_STARTUP,
    });
    SOL_CHECK_EQ_INT(T, rec.count, 1);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: Text buffer and event bus: UTF-8 multibyte roundtrip   */
/* ------------------------------------------------------------------ */

static void test_utf8_edit_events(SolTestCtx *T)
{
    IntegrationCtx ctx = make_ctx();

    U8Log log = {0};
    sol_event_bus_subscribe(ctx.bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
        .handler    = u8_handler,
        .user_data  = &log,
    });

    SolBufferId id = make_tb(&ctx, "u8");
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(ctx.sys, id));

    /* Insert 'é' (2 bytes) + '€' (3 bytes) + '𝄞' (4 bytes) = 9 bytes total. */
    sol_text_buffer_insert_codepoint(tb, 0x00E9u);  /* é */
    sol_text_buffer_insert_codepoint(tb, 0x20ACu);  /* € */
    sol_text_buffer_insert_codepoint(tb, 0x1D11Eu); /* 𝄞 */

    SOL_CHECK_EQ_INT(T, log.count, 3);
    SOL_CHECK_EQ_SZ(T, log.total_inserted, 9);

    free_ctx(&ctx);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_integration");

    SOL_RUN(s, test_lifecycle_events);
    SOL_RUN(s, test_focus_history);
    SOL_RUN(s, test_text_edit_events);
    SOL_RUN(s, test_event_type_name_roundtrip);
    SOL_RUN(s, test_split_cycle_leaf);
    SOL_RUN(s, test_queued_drain_integration);
    SOL_RUN(s, test_regression_close_active);
    SOL_RUN(s, test_regression_late_subscribe);
    SOL_RUN(s, test_utf8_edit_events);

    return sol_suite_report(&s);
}
