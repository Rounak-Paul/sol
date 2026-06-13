// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_plugin.c — Comprehensive unit tests for the Sol plugin system.
 *
 * Coverage (41 tests across 8 suites):
 *
 *   sol_plugin_manager  — create/destroy, registration, null-safety
 *   sol_plugin_metadata — id / display_name / version via ctx
 *   sol_plugin_unload   — unload by id, unload-all, double-unload
 *   sol_plugin_events   — subscribe/fire/unsub, auto-cleanup on unload, limit
 *   sol_plugin_services — versioned registry, auto-unregister, limit
 *   sol_plugin_buffers  — open_scratch, insert/read/delete, cursor, focus
 *   sol_plugin_status   — add/update/remove segments, auto-cleanup, limit
 *   sol_plugin_commands — register/unregister command flows, auto-cleanup
 *
 * NOTE: load_directory topo-sort is NOT tested here because it requires
 * real .dylib files on disk.  Topo-sort correctness is exercised at
 * integration level when plugins are loaded from a directory.
 */

#include "test_harness.h"

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "sol_system_manager.h"
#include "sol_event.h"
#include "sol_buffer.h"
#include "sol_text_buffer.h"

/* Private header lets us inspect SolUISystem fields directly — same
 * technique used by test_command_flow.c. */
#include "sol_ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* Test environment                                                    */
/* ================================================================== */

typedef struct TestEnv {
    SolSystemManager *sys;
    SolPluginManager *pm;   /* owned by sys; do NOT free separately   */
    SolUISystem      *ui;   /* calloc'd; primary_window stays NULL     */
} TestEnv;

static TestEnv make_env(void)
{
    TestEnv e;
    e.sys = sol_system_manager_create(NULL);
    e.pm  = sol_system_plugins(e.sys);
    e.ui  = (SolUISystem *)calloc(1, sizeof(SolUISystem));
    sol_plugin_manager_attach_ui(e.pm, e.ui);
    return e;
}

/* Destroy manager first so that plugin cleanup can still access ui,
 * then free the calloc'd ui stub. */
static void free_env(TestEnv *e)
{
    sol_system_manager_destroy(e->sys);
    e->sys = NULL;
    e->pm  = NULL;
    free(e->ui);
    e->ui  = NULL;
}

/* ================================================================== */
/* Shared plugin fixtures                                              */
/* ================================================================== */

/* A — minimal, no callbacks */
static const SolPluginAPI PLUGIN_A = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "test.a",
    .display_name = "Plugin A",
    .version      = "1.0.0",
};

/* B — tracks on_load / on_unload */
static SolPluginCtx *g_ctx_b    = NULL;
static int           g_load_b   = 0;
static int           g_unload_b = 0;

static bool plugin_b_load(SolPluginCtx *ctx)
{
    g_ctx_b = ctx;
    g_load_b++;
    return true;
}
static void plugin_b_unload(SolPluginCtx *ctx) { (void)ctx; g_unload_b++; }

static const SolPluginAPI PLUGIN_B = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "test.b",
    .display_name = "Plugin B",
    .version      = "2.0.0",
    .on_load      = plugin_b_load,
    .on_unload    = plugin_b_unload,
};

/* FAIL — on_load returns false */
static bool plugin_fail_load(SolPluginCtx *ctx) { (void)ctx; return false; }
static const SolPluginAPI PLUGIN_FAIL = {
    .api_version = SOL_PLUGIN_API_VERSION,
    .id          = "test.fail",
    .on_load     = plugin_fail_load,
};

/* BAD_VER — wrong API version */
static const SolPluginAPI PLUGIN_BAD_VER = {
    .api_version = SOL_PLUGIN_API_VERSION + 1u,
    .id          = "test.badver",
};

static void reset_globals(void)
{
    g_ctx_b    = NULL;
    g_load_b   = 0;
    g_unload_b = 0;
}

/* ================================================================== */
/* Suite 1: Manager lifecycle                                          */
/* ================================================================== */

static void test_create_destroy(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK_NOT_NULL(T, e.sys);
    SOL_CHECK_NOT_NULL(T, e.pm);
    free_env(&e);
}

static void test_count_empty(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    free_env(&e);
}

static void test_null_safety_manager(SolTestCtx *T)
{
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(NULL), 0u);
    SOL_CHECK(T, !sol_plugin_manager_register_static(NULL, &PLUGIN_A));
    SOL_CHECK(T, !sol_plugin_manager_register_static(NULL, NULL));
    SOL_CHECK(T, !sol_plugin_manager_load(NULL, "/no/such.dylib"));
    SOL_CHECK(T, !sol_plugin_manager_unload(NULL, "anything"));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_unload_all(NULL), 0u);
    SOL_CHECK(T, !sol_plugin_manager_reload(NULL, "anything"));
    sol_plugin_manager_attach_ui(NULL, NULL);  /* must not crash */
    sol_plugin_manager_destroy(NULL);          /* must not crash */
}

static void test_register_basic(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 1u);
    free_env(&e);
}

static void test_register_on_load_called(SolTestCtx *T)
{
    reset_globals();
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_B));
    SOL_CHECK_EQ_INT(T, g_load_b, 1);
    SOL_CHECK_NOT_NULL(T, g_ctx_b);
    free_env(&e);
    reset_globals();
}

static void test_register_on_load_false(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T, !sol_plugin_manager_register_static(e.pm, &PLUGIN_FAIL));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    free_env(&e);
}

static void test_register_wrong_version(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T, !sol_plugin_manager_register_static(e.pm, &PLUGIN_BAD_VER));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    free_env(&e);
}

static void test_register_null_id(SolTestCtx *T)
{
    TestEnv e = make_env();
    SolPluginAPI no_id = { .api_version = SOL_PLUGIN_API_VERSION, .id = NULL };
    SOL_CHECK(T, !sol_plugin_manager_register_static(e.pm, &no_id));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    free_env(&e);
}

static void test_register_duplicate(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T,  sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK(T, !sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 1u);
    free_env(&e);
}

static void test_register_null_on_load(SolTestCtx *T)
{
    /* NULL on_load is valid — plugin just does no setup */
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 1u);
    free_env(&e);
}

/* ================================================================== */
/* Suite 2: Plugin metadata via ctx                                   */
/* ================================================================== */

static SolPluginCtx *g_meta_ctx = NULL;
static bool meta_plugin_load(SolPluginCtx *ctx) { g_meta_ctx = ctx; return true; }

static void test_metadata_full(SolTestCtx *T)
{
    g_meta_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version  = SOL_PLUGIN_API_VERSION,
        .id           = "test.meta",
        .display_name = "Meta Plugin",
        .version      = "3.1.4",
        .on_load      = meta_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));
    SOL_CHECK_NOT_NULL(T, g_meta_ctx);
    SOL_CHECK_STR(T, sol_plugin_id(g_meta_ctx),           "test.meta");
    SOL_CHECK_STR(T, sol_plugin_display_name(g_meta_ctx), "Meta Plugin");
    SOL_CHECK_STR(T, sol_plugin_version(g_meta_ctx),      "3.1.4");
    free_env(&e);
    g_meta_ctx = NULL;
}

static void test_metadata_defaults(SolTestCtx *T)
{
    g_meta_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version  = SOL_PLUGIN_API_VERSION,
        .id           = "test.defaults",
        .display_name = NULL,   /* → defaults to id */
        .version      = NULL,   /* → defaults to "0.0.0" */
        .on_load      = meta_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));
    SOL_CHECK_STR(T, sol_plugin_display_name(g_meta_ctx), "test.defaults");
    SOL_CHECK_STR(T, sol_plugin_version(g_meta_ctx),      "0.0.0");
    free_env(&e);
    g_meta_ctx = NULL;
}

static void test_metadata_null_ctx(SolTestCtx *T)
{
    SOL_CHECK_NULL(T, sol_plugin_id(NULL));
    SOL_CHECK_NULL(T, sol_plugin_display_name(NULL));
    SOL_CHECK_NULL(T, sol_plugin_version(NULL));
    SOL_CHECK_NULL(T, sol_plugin_systems(NULL));
    SOL_CHECK_NULL(T, sol_plugin_event_bus(NULL));
    SOL_CHECK_NULL(T, sol_plugin_buffers(NULL));
    SOL_CHECK_NULL(T, sol_plugin_jobs(NULL));
    SOL_CHECK_NULL(T, sol_plugin_input(NULL));
    SOL_CHECK_NULL(T, sol_plugin_ui(NULL));
}

/* ================================================================== */
/* Suite 3: Unload                                                     */
/* ================================================================== */

static void test_unload_by_id(SolTestCtx *T)
{
    reset_globals();
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_B));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 1u);
    SOL_CHECK(T, sol_plugin_manager_unload(e.pm, "test.b"));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    SOL_CHECK_EQ_INT(T, g_unload_b, 1);
    free_env(&e);
    reset_globals();
}

static void test_unload_unknown_id(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK(T, !sol_plugin_manager_unload(e.pm, "does.not.exist"));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 1u);
    free_env(&e);
}

static void test_unload_all(SolTestCtx *T)
{
    reset_globals();
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_B));
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 2u);
    const size_t n = sol_plugin_manager_unload_all(e.pm);
    SOL_CHECK_EQ_SZ(T, n, 2u);
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    SOL_CHECK_EQ_INT(T, g_unload_b, 1);
    free_env(&e);
    reset_globals();
}

static void test_double_unload(SolTestCtx *T)
{
    TestEnv e = make_env();
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &PLUGIN_A));
    SOL_CHECK(T,  sol_plugin_manager_unload(e.pm, "test.a"));
    SOL_CHECK(T, !sol_plugin_manager_unload(e.pm, "test.a")); /* already gone */
    SOL_CHECK_EQ_SZ(T, sol_plugin_manager_count(e.pm), 0u);
    free_env(&e);
}

/* ================================================================== */
/* Suite 4: Event subscriptions                                        */
/* ================================================================== */

typedef struct SubRecord { int count; } SubRecord;

static bool sub_record_handler(const SolEvent *e, void *ud)
{
    (void)e;
    ((SubRecord *)ud)->count++;
    return false;
}

static SolPluginCtx *g_sub_ctx = NULL;
static bool sub_plugin_load(SolPluginCtx *ctx) { g_sub_ctx = ctx; return true; }
static void sub_plugin_unload(SolPluginCtx *ctx) { (void)ctx; }

static void test_subscribe_fires(SolTestCtx *T)
{
    g_sub_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.sub",
        .on_load     = sub_plugin_load,
        .on_unload   = sub_plugin_unload,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SubRecord rec = {0};
    SolSubscriptionToken tok = sol_plugin_subscribe(
        g_sub_ctx, "test.custom.event", sub_record_handler, &rec);
    SOL_CHECK_MSG(T, tok != 0u, "subscribe returned invalid token");

    sol_event_bus_publish(sol_system_events(e.sys), &(SolEventDesc){
        .event_name = "test.custom.event",
    });
    SOL_CHECK_EQ_INT(T, rec.count, 1);

    free_env(&e);
    g_sub_ctx = NULL;
}

static void test_unsubscribe(SolTestCtx *T)
{
    g_sub_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.sub2",
        .on_load     = sub_plugin_load,
        .on_unload   = sub_plugin_unload,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SubRecord rec = {0};
    SolSubscriptionToken tok = sol_plugin_subscribe(
        g_sub_ctx, "test.custom.event2", sub_record_handler, &rec);
    SolEventBus *bus = sol_system_events(e.sys);

    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = "test.custom.event2" });
    SOL_CHECK_EQ_INT(T, rec.count, 1);

    sol_plugin_unsubscribe(g_sub_ctx, tok);
    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = "test.custom.event2" });
    SOL_CHECK_EQ_INT(T, rec.count, 1); /* still 1 — no second fire */

    free_env(&e);
    g_sub_ctx = NULL;
}

static void test_auto_unsub_on_unload(SolTestCtx *T)
{
    g_sub_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.sub3",
        .on_load     = sub_plugin_load,
        .on_unload   = sub_plugin_unload,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SubRecord rec = {0};
    sol_plugin_subscribe(g_sub_ctx, "test.custom.event3", sub_record_handler, &rec);

    SolEventBus *bus = sol_system_events(e.sys);
    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = "test.custom.event3" });
    SOL_CHECK_EQ_INT(T, rec.count, 1);

    sol_plugin_manager_unload(e.pm, "test.sub3");
    g_sub_ctx = NULL; /* ptr is now freed — do NOT dereference */

    sol_event_bus_publish(bus, &(SolEventDesc){ .event_name = "test.custom.event3" });
    SOL_CHECK_EQ_INT(T, rec.count, 1); /* still 1 — handler auto-removed */

    free_env(&e);
}

static void test_subscribe_limit(SolTestCtx *T)
{
    g_sub_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.sublimit",
        .on_load     = sub_plugin_load,
        .on_unload   = sub_plugin_unload,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int success = 0;
    for (int i = 0; i < 64; ++i) {
        SolSubscriptionToken t = sol_plugin_subscribe(
            g_sub_ctx, "test.dummy", sub_record_handler, NULL);
        if (t != 0u) ++success;
    }
    SOL_CHECK_EQ_INT(T, success, 64);

    /* 65th must fail at the ctx level */
    SolSubscriptionToken over = sol_plugin_subscribe(
        g_sub_ctx, "test.dummy", sub_record_handler, NULL);
    SOL_CHECK_MSG(T, over == 0u, "65th subscription should return 0");

    free_env(&e);
    g_sub_ctx = NULL;
}

/* ================================================================== */
/* Suite 5: Service registry                                           */
/* ================================================================== */

static SolPluginCtx *g_svc_ctx = NULL;
static bool svc_plugin_load(SolPluginCtx *ctx) { g_svc_ctx = ctx; return true; }

static void test_service_register_get(SolTestCtx *T)
{
    g_svc_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.svc",
        .on_load     = svc_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int dummy = 42;
    SOL_CHECK(T, sol_plugin_register_service(g_svc_ctx, "test.my_service",
                                              0u, &dummy, NULL, NULL));
    void *got = sol_plugin_get_service(g_svc_ctx, "test.my_service", 0u);
    SOL_CHECK_MSG(T, got == &dummy, "retrieved service pointer mismatch");

    free_env(&e);
    g_svc_ctx = NULL;
}

static void test_service_versioned(SolTestCtx *T)
{
    g_svc_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.svc2",
        .on_load     = svc_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int dummy = 99;
    SOL_CHECK(T, sol_plugin_register_service(g_svc_ctx, "test.versioned_svc",
                                              3u, &dummy, NULL, NULL));

    /* min_version <= registered version → returns service */
    void *ok = sol_plugin_get_service(g_svc_ctx, "test.versioned_svc", 2u);
    SOL_CHECK_MSG(T, ok == &dummy, "versioned service should be returned for min_ver=2");

    /* min_version == registered version → also OK */
    void *exact = sol_plugin_get_service(g_svc_ctx, "test.versioned_svc", 3u);
    SOL_CHECK_MSG(T, exact == &dummy, "versioned service should match exact version");

    /* min_version > registered version → NULL */
    void *fail = sol_plugin_get_service(g_svc_ctx, "test.versioned_svc", 4u);
    SOL_CHECK_MSG(T, fail == NULL, "too-high min_version should return NULL");

    free_env(&e);
    g_svc_ctx = NULL;
}

static void test_service_auto_unreg(SolTestCtx *T)
{
    g_svc_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.svc3",
        .on_load     = svc_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int dummy = 7;
    sol_plugin_register_service(g_svc_ctx, "test.autounreg_svc",
                                  0u, &dummy, NULL, NULL);

    void *before = sol_system_get_service(e.sys, "test.autounreg_svc");
    SOL_CHECK_MSG(T, before == &dummy, "service not registered in system manager");

    sol_plugin_manager_unload(e.pm, "test.svc3");
    g_svc_ctx = NULL;

    void *after = sol_system_get_service(e.sys, "test.autounreg_svc");
    SOL_CHECK_MSG(T, after == NULL, "service should be gone after plugin unload");

    free_env(&e);
}

static void test_service_limit(SolTestCtx *T)
{
    g_svc_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.svclimit",
        .on_load     = svc_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int dummies[17] = {0};
    int registered = 0;
    char name[64];
    for (int i = 0; i < 16; ++i) {
        snprintf(name, sizeof(name), "test.limit_svc_%d", i);
        if (sol_plugin_register_service(g_svc_ctx, name, 0u,
                                         &dummies[i], NULL, NULL))
            ++registered;
    }
    SOL_CHECK_EQ_INT(T, registered, 16);

    /* 17th must fail at the ctx limit */
    SOL_CHECK(T, !sol_plugin_register_service(g_svc_ctx,
                                               "test.limit_svc_overflow",
                                               0u, &dummies[16], NULL, NULL));
    free_env(&e);
    g_svc_ctx = NULL;
}

/* ================================================================== */
/* Suite 6: Buffer operations                                          */
/* ================================================================== */

static SolPluginCtx *g_buf_ctx = NULL;
static bool buf_plugin_load(SolPluginCtx *ctx) { g_buf_ctx = ctx; return true; }

static void test_buf_open_scratch(SolTestCtx *T)
{
    g_buf_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.buf",
        .on_load     = buf_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolBufferId id = sol_plugin_open_scratch(g_buf_ctx, "scratch",
                                              "hello", 5u, NULL);
    SOL_CHECK_MSG(T, id != 0u, "open_scratch returned 0");
    SOL_CHECK_EQ_SZ(T, sol_plugin_buf_length(g_buf_ctx, id), 5u);

    free_env(&e);
    g_buf_ctx = NULL;
}

static void test_buf_insert_read(SolTestCtx *T)
{
    g_buf_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.buf2",
        .on_load     = buf_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolBufferId id = sol_plugin_open_scratch(g_buf_ctx, "s2", NULL, 0u, NULL);
    SOL_CHECK_MSG(T, id != 0u, "open_scratch returned 0");

    SOL_CHECK(T, sol_plugin_buf_insert(g_buf_ctx, id, 0u, "hello", 5u));

    char buf[32] = {0};
    size_t n = sol_plugin_buf_read(g_buf_ctx, id, 0u, buf, sizeof(buf));
    SOL_CHECK_EQ_SZ(T, n, 5u);
    SOL_CHECK_STR(T, buf, "hello");

    free_env(&e);
    g_buf_ctx = NULL;
}

static void test_buf_delete(SolTestCtx *T)
{
    g_buf_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.buf3",
        .on_load     = buf_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolBufferId id = sol_plugin_open_scratch(g_buf_ctx, "s3",
                                              "hello world", 11u, NULL);
    /* delete " world" (bytes 5..10 inclusive = offset 5, count 6) */
    SOL_CHECK(T, sol_plugin_buf_delete(g_buf_ctx, id, 5u, 6u));

    char buf[32] = {0};
    sol_plugin_buf_read(g_buf_ctx, id, 0u, buf, sizeof(buf));
    SOL_CHECK_STR(T, buf, "hello");
    SOL_CHECK_EQ_SZ(T, sol_plugin_buf_length(g_buf_ctx, id), 5u);

    free_env(&e);
    g_buf_ctx = NULL;
}

static void test_buf_cursor(SolTestCtx *T)
{
    g_buf_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.buf4",
        .on_load     = buf_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolBufferId id = sol_plugin_open_scratch(g_buf_ctx, "s4", "abcde", 5u, NULL);
    SOL_CHECK(T, sol_plugin_buf_set_cursor(g_buf_ctx, id, 3u));
    SOL_CHECK_EQ_SZ(T, sol_plugin_buf_cursor(g_buf_ctx, id), 3u);

    free_env(&e);
    g_buf_ctx = NULL;
}

static void test_buf_focus_active(SolTestCtx *T)
{
    g_buf_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.buf5",
        .on_load     = buf_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolBufferId id = sol_plugin_open_scratch(g_buf_ctx, "s5", NULL, 0u, NULL);
    SOL_CHECK(T, sol_plugin_focus_buffer(g_buf_ctx, id));
    SOL_CHECK_MSG(T, sol_plugin_active_buffer(g_buf_ctx) == id,
                  "active buffer did not match focused id");

    free_env(&e);
    g_buf_ctx = NULL;
}

static void test_buf_null_safety(SolTestCtx *T)
{
    SOL_CHECK_EQ_SZ(T, sol_plugin_buf_length(NULL, 0u), 0u);
    SOL_CHECK_EQ_SZ(T, sol_plugin_buf_cursor(NULL, 0u), 0u);
    SOL_CHECK(T, !sol_plugin_buf_set_cursor(NULL, 0u, 0u));
    SOL_CHECK(T, !sol_plugin_buf_insert(NULL, 0u, 0u, "x", 1u));
    SOL_CHECK(T, !sol_plugin_buf_delete(NULL, 0u, 0u, 1u));
    SOL_CHECK(T, !sol_plugin_focus_buffer(NULL, 0u));
    SOL_CHECK_MSG(T, sol_plugin_active_buffer(NULL) == 0u,
                  "active_buffer(NULL) should return 0");
    SOL_CHECK_MSG(T, sol_plugin_open_file(NULL, "/no") == 0u,
                  "open_file(NULL ctx) should return 0");
    SOL_CHECK_MSG(T, sol_plugin_open_scratch(NULL, "x", NULL, 0u, NULL) == 0u,
                  "open_scratch(NULL ctx) should return 0");
}

/* ================================================================== */
/* Suite 7: Status bar segments                                        */
/* ================================================================== */

static SolPluginCtx *g_st_ctx = NULL;
static bool st_plugin_load(SolPluginCtx *ctx) { g_st_ctx = ctx; return true; }

static void test_status_add_update_remove(SolTestCtx *T)
{
    g_st_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.status",
        .on_load     = st_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolPluginStatusToken tok = sol_plugin_add_status_segment(g_st_ctx, "v1", NULL);
    SOL_CHECK_MSG(T, tok != SOL_PLUGIN_STATUS_TOKEN_INVALID,
                  "add_status_segment returned INVALID");

    /* Verify text stored in UI state */
    bool found_v1 = false;
    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        SolUIStatusSegment *seg = &e.ui->plugin_status_segs[i];
        if (seg->in_use && seg->token == (SolUIStatusToken)tok)
            found_v1 = (strcmp(seg->text, "v1") == 0);
    }
    SOL_CHECK_MSG(T, found_v1, "segment text 'v1' not stored in ui state");

    sol_plugin_update_status_segment(g_st_ctx, tok, "v2");

    bool found_v2 = false;
    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        SolUIStatusSegment *seg = &e.ui->plugin_status_segs[i];
        if (seg->in_use && seg->token == (SolUIStatusToken)tok)
            found_v2 = (strcmp(seg->text, "v2") == 0);
    }
    SOL_CHECK_MSG(T, found_v2, "segment text not updated to 'v2'");

    sol_plugin_remove_status_segment(g_st_ctx, tok);

    bool still_there = false;
    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i)
        if (e.ui->plugin_status_segs[i].in_use &&
            e.ui->plugin_status_segs[i].token == (SolUIStatusToken)tok)
            still_there = true;
    SOL_CHECK_MSG(T, !still_there, "segment still present after remove");

    free_env(&e);
    g_st_ctx = NULL;
}

static void test_status_auto_remove(SolTestCtx *T)
{
    g_st_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.status2",
        .on_load     = st_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    SolPluginStatusToken tok = sol_plugin_add_status_segment(g_st_ctx, "seg", NULL);
    SOL_CHECK_MSG(T, tok != SOL_PLUGIN_STATUS_TOKEN_INVALID, "add_status_segment failed");

    sol_plugin_manager_unload(e.pm, "test.status2");
    g_st_ctx = NULL;

    bool still_there = false;
    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i)
        if (e.ui->plugin_status_segs[i].in_use) still_there = true;
    SOL_CHECK_MSG(T, !still_there, "segment still present after plugin unload");

    free_env(&e);
}

static void test_status_invalid_ops(SolTestCtx *T)
{
    /* Operations with INVALID token and NULL ctx must not crash */
    sol_plugin_update_status_segment(NULL, SOL_PLUGIN_STATUS_TOKEN_INVALID, "x");
    sol_plugin_remove_status_segment(NULL, SOL_PLUGIN_STATUS_TOKEN_INVALID);

    g_st_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.status3",
        .on_load     = st_plugin_load,
    };
    sol_plugin_manager_register_static(e.pm, &api);
    /* Valid ctx, invalid token — must not crash or corrupt state */
    sol_plugin_update_status_segment(g_st_ctx, SOL_PLUGIN_STATUS_TOKEN_INVALID, "x");
    sol_plugin_remove_status_segment(g_st_ctx, SOL_PLUGIN_STATUS_TOKEN_INVALID);
    SOL_CHECK(T, true); /* reaching here = no crash */

    free_env(&e);
    g_st_ctx = NULL;
}

static void test_status_limit(SolTestCtx *T)
{
    g_st_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.statuslim",
        .on_load     = st_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int added = 0;
    for (int i = 0; i < 8; ++i) {
        SolPluginStatusToken t = sol_plugin_add_status_segment(g_st_ctx, "x", NULL);
        if (t != SOL_PLUGIN_STATUS_TOKEN_INVALID) ++added;
    }
    SOL_CHECK_EQ_INT(T, added, 8);

    /* 9th must fail — ctx limit reached */
    SolPluginStatusToken over = sol_plugin_add_status_segment(g_st_ctx, "overflow", NULL);
    SOL_CHECK_MSG(T, over == SOL_PLUGIN_STATUS_TOKEN_INVALID,
                  "9th segment should return INVALID (ctx limit is 8)");

    free_env(&e);
    g_st_ctx = NULL;
}

/* ================================================================== */
/* Suite 8: Command flow registration                                  */
/* ================================================================== */

static SolPluginCtx *g_cmd_ctx = NULL;
static bool cmd_plugin_load(SolPluginCtx *ctx) { g_cmd_ctx = ctx; return true; }

static bool dummy_cmd_cb(const char *action, const SolInputEvent *ev, void *ud)
{
    (void)action; (void)ev; (void)ud; return false;
}

/* One-key chord arrays used by the command tests. */
static const SolKeyCode chord_z[] = { (SolKeyCode)'Z' };
static const SolKeyCode chord_y[] = { (SolKeyCode)'Y' };
static const SolKeyCode chord_x[] = { (SolKeyCode)'X' };

static void test_command_register(SolTestCtx *T)
{
    g_cmd_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.cmd",
        .on_load     = cmd_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    int flows_before = e.ui->command_flow_count;
    bool ok = sol_plugin_register_command(g_cmd_ctx, &(SolPluginCommandDesc){
        .action       = "test.cmd.hello",
        .label        = "Say Hello",
        .chord        = chord_z,
        .chord_length = 1u,
        .callback     = dummy_cmd_cb,
    });
    SOL_CHECK_MSG(T, ok, "register_command returned false");
    SOL_CHECK_MSG(T, e.ui->command_flow_count == flows_before + 1,
                  "command_flow_count did not increase after register");

    free_env(&e);
    g_cmd_ctx = NULL;
}

static void test_command_unregister(SolTestCtx *T)
{
    g_cmd_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.cmd2",
        .on_load     = cmd_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    sol_plugin_register_command(g_cmd_ctx, &(SolPluginCommandDesc){
        .action       = "test.cmd2.flow",
        .chord        = chord_y,
        .chord_length = 1u,
        .callback     = dummy_cmd_cb,
    });
    int after_reg = e.ui->command_flow_count;

    sol_plugin_unregister_command(g_cmd_ctx, "test.cmd2.flow");
    SOL_CHECK_MSG(T, e.ui->command_flow_count == after_reg - 1,
                  "command_flow_count did not decrease after explicit unregister");

    free_env(&e);
    g_cmd_ctx = NULL;
}

static void test_command_auto_unreg(SolTestCtx *T)
{
    g_cmd_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id          = "test.cmd3",
        .on_load     = cmd_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));

    sol_plugin_register_command(g_cmd_ctx, &(SolPluginCommandDesc){
        .action       = "test.cmd3.flow",
        .chord        = chord_x,
        .chord_length = 1u,
        .callback     = dummy_cmd_cb,
    });
    int after_reg = e.ui->command_flow_count;

    sol_plugin_manager_unload(e.pm, "test.cmd3");
    g_cmd_ctx = NULL;

    SOL_CHECK_MSG(T, e.ui->command_flow_count == after_reg - 1,
                  "command flow not auto-unregistered on plugin unload");

    free_env(&e);
}

/* ================================================================== */
/* Suite 9: Side panel contributions                                   */
/* ================================================================== */

static SolPluginCtx *g_panel_ctx = NULL;
static bool panel_plugin_load(SolPluginCtx *ctx) { g_panel_ctx = ctx; return true; }
static void dummy_panel_render(void *user_data) { (void)user_data; }

static void test_side_panel_lifecycle(SolTestCtx *T)
{
    g_panel_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id = "test.panel",
        .on_load = panel_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));
    SolPluginSidePanelToken token = sol_plugin_register_side_panel(
        g_panel_ctx,
        &(SolPluginSidePanelDesc){
            .id = "test.panel.sidebar",
            .title = "Test Panel",
            .render = dummy_panel_render,
        });
    SOL_CHECK(T, token != SOL_PLUGIN_SIDE_PANEL_TOKEN_INVALID);
    SOL_CHECK(T, sol_plugin_show_side_panel(g_panel_ctx, token));
    SOL_CHECK(T, sol_plugin_side_panel_visible(g_panel_ctx, token));
    sol_plugin_hide_side_panel(g_panel_ctx, token);
    SOL_CHECK(T, !sol_plugin_side_panel_visible(g_panel_ctx, token));
    sol_plugin_unregister_side_panel(g_panel_ctx, token);

    bool any_panel = false;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i)
        if (e.ui->side_panels[i].in_use) any_panel = true;
    SOL_CHECK_MSG(T, !any_panel, "side panel still registered after explicit removal");
    free_env(&e);
    g_panel_ctx = NULL;
}

static void test_side_panel_auto_remove(SolTestCtx *T)
{
    g_panel_ctx = NULL;
    TestEnv e = make_env();
    SolPluginAPI api = {
        .api_version = SOL_PLUGIN_API_VERSION,
        .id = "test.panel.auto",
        .on_load = panel_plugin_load,
    };
    SOL_CHECK(T, sol_plugin_manager_register_static(e.pm, &api));
    SolPluginSidePanelToken token = sol_plugin_register_side_panel(
        g_panel_ctx,
        &(SolPluginSidePanelDesc){
            .id = "test.panel.auto.sidebar",
            .render = dummy_panel_render,
        });
    SOL_CHECK(T, token != SOL_PLUGIN_SIDE_PANEL_TOKEN_INVALID);
    SOL_CHECK(T, sol_plugin_show_side_panel(g_panel_ctx, token));
    SOL_CHECK(T, sol_plugin_manager_unload(e.pm, "test.panel.auto"));

    bool any_panel = false;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i)
        if (e.ui->side_panels[i].in_use) any_panel = true;
    SOL_CHECK_MSG(T, !any_panel, "side panel still registered after plugin unload");
    SOL_CHECK(T, e.ui->active_side_panel == SOL_UI_SIDE_PANEL_TOKEN_INVALID);
    free_env(&e);
    g_panel_ctx = NULL;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
    SolTestSuite *suites = (SolTestSuite *)calloc(9u, sizeof(SolTestSuite));
    if (!suites) return 1;
    int si = 0;

    /* Suite 1 — manager */
    sol_suite_init(&suites[si], "sol_plugin_manager");
    SOL_RUN(suites[si], test_create_destroy);
    SOL_RUN(suites[si], test_count_empty);
    SOL_RUN(suites[si], test_null_safety_manager);
    SOL_RUN(suites[si], test_register_basic);
    SOL_RUN(suites[si], test_register_on_load_called);
    SOL_RUN(suites[si], test_register_on_load_false);
    SOL_RUN(suites[si], test_register_wrong_version);
    SOL_RUN(suites[si], test_register_null_id);
    SOL_RUN(suites[si], test_register_duplicate);
    SOL_RUN(suites[si], test_register_null_on_load);
    sol_suite_report(&suites[si++]);

    /* Suite 2 — metadata */
    sol_suite_init(&suites[si], "sol_plugin_metadata");
    SOL_RUN(suites[si], test_metadata_full);
    SOL_RUN(suites[si], test_metadata_defaults);
    SOL_RUN(suites[si], test_metadata_null_ctx);
    sol_suite_report(&suites[si++]);

    /* Suite 3 — unload */
    sol_suite_init(&suites[si], "sol_plugin_unload");
    SOL_RUN(suites[si], test_unload_by_id);
    SOL_RUN(suites[si], test_unload_unknown_id);
    SOL_RUN(suites[si], test_unload_all);
    SOL_RUN(suites[si], test_double_unload);
    sol_suite_report(&suites[si++]);

    /* Suite 4 — subscriptions */
    sol_suite_init(&suites[si], "sol_plugin_subscriptions");
    SOL_RUN(suites[si], test_subscribe_fires);
    SOL_RUN(suites[si], test_unsubscribe);
    SOL_RUN(suites[si], test_auto_unsub_on_unload);
    SOL_RUN(suites[si], test_subscribe_limit);
    sol_suite_report(&suites[si++]);

    /* Suite 5 — services */
    sol_suite_init(&suites[si], "sol_plugin_services");
    SOL_RUN(suites[si], test_service_register_get);
    SOL_RUN(suites[si], test_service_versioned);
    SOL_RUN(suites[si], test_service_auto_unreg);
    SOL_RUN(suites[si], test_service_limit);
    sol_suite_report(&suites[si++]);

    /* Suite 6 — buffer ops */
    sol_suite_init(&suites[si], "sol_plugin_buffers");
    SOL_RUN(suites[si], test_buf_open_scratch);
    SOL_RUN(suites[si], test_buf_insert_read);
    SOL_RUN(suites[si], test_buf_delete);
    SOL_RUN(suites[si], test_buf_cursor);
    SOL_RUN(suites[si], test_buf_focus_active);
    SOL_RUN(suites[si], test_buf_null_safety);
    sol_suite_report(&suites[si++]);

    /* Suite 7 — status bar */
    sol_suite_init(&suites[si], "sol_plugin_status");
    SOL_RUN(suites[si], test_status_add_update_remove);
    SOL_RUN(suites[si], test_status_auto_remove);
    SOL_RUN(suites[si], test_status_invalid_ops);
    SOL_RUN(suites[si], test_status_limit);
    sol_suite_report(&suites[si++]);

    /* Suite 8 — commands */
    sol_suite_init(&suites[si], "sol_plugin_commands");
    SOL_RUN(suites[si], test_command_register);
    SOL_RUN(suites[si], test_command_unregister);
    SOL_RUN(suites[si], test_command_auto_unreg);
    sol_suite_report(&suites[si++]);

    /* Suite 9 — side panels */
    sol_suite_init(&suites[si], "sol_plugin_side_panels");
    SOL_RUN(suites[si], test_side_panel_lifecycle);
    SOL_RUN(suites[si], test_side_panel_auto_remove);
    sol_suite_report(&suites[si++]);

    const int result = sol_report_all(suites, si);
    free(suites);
    return result;
}
