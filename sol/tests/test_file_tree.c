// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "test_harness.h"

#include "sol_event.h"
#include "sol_file_tree.h"

typedef struct RootEventLog {
    int count;
    const char *last_path;
} RootEventLog;

static bool root_event_handler(const SolEvent *event, void *user_data)
{
    RootEventLog *log = (RootEventLog *)user_data;
    if (!log || !event || !event->payload ||
        event->payload_size < sizeof(SolFileTreeRootPayload)) {
        return false;
    }
    const SolFileTreeRootPayload *payload =
        (const SolFileTreeRootPayload *)event->payload;
    log->last_path = payload->path;
    log->count++;
    return false;
}

static void test_root_clear_notifies_and_resets(SolTestCtx *T)
{
    SolEventBusConfig ecfg = sol_event_bus_config_default();
    SolEventBus *bus = sol_event_bus_create(&ecfg);
    SOL_CHECK_NOT_NULL(T, bus);
    if (!bus) return;

    SolFileTree *tree = sol_file_tree_create();
    SOL_CHECK_NOT_NULL(T, tree);
    if (!tree) {
        sol_event_bus_destroy(bus);
        return;
    }

    sol_file_tree_attach_event_bus(tree, bus);

    RootEventLog log = {0};
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_FILE_TREE_ROOT,
        .handler    = root_event_handler,
        .user_data  = &log,
    });

    SOL_CHECK(T, sol_file_tree_set_root(tree, "."));
    SOL_CHECK_STR(T, sol_file_tree_root(tree), ".");
    SOL_CHECK(T, log.count == 1);
    SOL_CHECK_NOT_NULL(T, log.last_path);

    log.last_path = (const char *)0x1;
    SOL_CHECK(T, !sol_file_tree_set_root(tree, "/definitely/not/a/real/sol/tree"));
    SOL_CHECK_NULL(T, sol_file_tree_root(tree));
    SOL_CHECK_EQ_SZ(T, sol_file_tree_visible_count(tree), 0u);
    SOL_CHECK(T, log.count == 2);
    SOL_CHECK_NULL(T, log.last_path);

    sol_file_tree_destroy(tree);
    sol_event_bus_destroy(bus);
}

int main(void)
{
    SolTestSuite suite;
    sol_suite_init(&suite, "sol_file_tree_tests");
    SOL_RUN(suite, test_root_clear_notifies_and_resets);
    return sol_suite_report(&suite);
}
