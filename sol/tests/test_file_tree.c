// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "test_harness.h"

#include "sol_event.h"
#include "sol_file_tree.h"
#include "sol_platform.h"

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

static bool write_file_bytes(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    const size_t len = strlen(text);
    const bool ok = fwrite(text, 1u, len, fp) == len;
    fclose(fp);
    return ok;
}

/*
 * Regression: sol_file_tree_refresh must NOT publish SOL_EVENT_FILE_TREE_ROOT
 * (unlike sol_file_tree_set_root). It exists specifically for high-frequency
 * callers like a filesystem watcher's drain loop, which re-scan the same
 * root on every external change; publishing the root-changed event on every
 * call caused subscribers that treat that event as "the workspace changed,
 * discard and rebuild everything" (e.g. the git plugin re-running full
 * discovery) to thrash continuously. This also verifies refresh picks up a
 * newly-created file and preserves expansion state across the re-scan.
 */
static void test_refresh_does_not_publish_root_event(SolTestCtx *T)
{
    SolEventBusConfig ecfg = sol_event_bus_config_default();
    SolEventBus *bus = sol_event_bus_create(&ecfg);
    SOL_CHECK_NOT_NULL(T, bus);
    if (!bus) return;

    SolFileTree *tree = sol_file_tree_create();
    SOL_CHECK_NOT_NULL(T, tree);
    if (!tree) { sol_event_bus_destroy(bus); return; }
    sol_file_tree_attach_event_bus(tree, bus);

    RootEventLog log = {0};
    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_FILE_TREE_ROOT,
        .handler    = root_event_handler,
        .user_data  = &log,
    });

    char root[256];
    snprintf(root, sizeof(root), "/tmp/sol_tree_refresh_%llu",
             (unsigned long long)sol_test_now_ns());
    char *sub_dir = sol_platform_path_join(root, "sub");
    SOL_CHECK_NOT_NULL(T, sub_dir);
    if (!sub_dir) { sol_file_tree_destroy(tree); sol_event_bus_destroy(bus); return; }
    SOL_CHECK(T, sol_platform_mkdir_p(sub_dir));

    SOL_CHECK(T, sol_file_tree_set_root(tree, root));
    SOL_CHECK(T, log.count == 1);   /* set_root: exactly one root-changed event */

    /* Find and expand the "sub" row so refresh has expansion state to
       preserve, mirroring what a user who had a folder open would see. */
    size_t sub_index = (size_t)-1;
    for (size_t i = 0; i < sol_file_tree_visible_count(tree); ++i) {
        const SolFileEntry *e = sol_file_tree_visible(tree, i);
        if (e && e->is_dir && strcmp(e->name, "sub") == 0) { sub_index = i; break; }
    }
    SOL_CHECK(T, sub_index != (size_t)-1);
    if (sub_index != (size_t)-1) {
        SOL_CHECK(T, sol_file_tree_toggle(tree, sub_index));
        const SolFileEntry *e = sol_file_tree_visible(tree, sub_index);
        SOL_CHECK(T, e && e->expanded);
    }

    /* An external change lands on disk, then the watcher-style caller
       re-scans via refresh — this must not touch the event bus. */
    char *new_file = sol_platform_path_join(sub_dir, "new.txt");
    SOL_CHECK(T, write_file_bytes(new_file, "hi"));
    SOL_CHECK(T, sol_file_tree_refresh(tree));
    SOL_CHECK(T, log.count == 1);   /* still 1 — refresh publishes nothing */

    /* The new file is now visible, and "sub" is still expanded. */
    bool found_new_file = false;
    bool sub_still_expanded = false;
    for (size_t i = 0; i < sol_file_tree_visible_count(tree); ++i) {
        const SolFileEntry *e = sol_file_tree_visible(tree, i);
        if (!e) continue;
        if (!e->is_dir && strcmp(e->name, "new.txt") == 0) found_new_file = true;
        if (e->is_dir && strcmp(e->name, "sub") == 0 && e->expanded) sub_still_expanded = true;
    }
    SOL_CHECK(T, found_new_file);
    SOL_CHECK(T, sub_still_expanded);

    free(new_file);
    free(sub_dir);
    sol_file_tree_destroy(tree);
    sol_event_bus_destroy(bus);
    (void)sol_platform_remove_path_recursive(root);
}

static bool read_file_bytes(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0u) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    const size_t n = fread(out, 1u, out_size - 1u, fp);
    out[n] = '\0';
    const bool ok = !ferror(fp);
    fclose(fp);
    return ok;
}

static void test_platform_recursive_file_ops(SolTestCtx *T)
{
    char root[256];
    snprintf(root, sizeof(root), "/tmp/sol_platform_fs_%llu",
             (unsigned long long)sol_test_now_ns());
    char *src_dir = sol_platform_path_join(root, "src");
    char *nested = sol_platform_path_join(src_dir, "nested");
    char *file = sol_platform_path_join(nested, "a.txt");
    char *copy_dir = sol_platform_path_join(root, "copy");
    char *copy_nested = sol_platform_path_join(copy_dir, "nested");
    char *copy_file = sol_platform_path_join(copy_nested, "a.txt");
    char *move_file = sol_platform_path_join(root, "moved.txt");

    SOL_CHECK_NOT_NULL(T, src_dir);
    SOL_CHECK_NOT_NULL(T, nested);
    SOL_CHECK_NOT_NULL(T, file);
    SOL_CHECK_NOT_NULL(T, copy_dir);
    SOL_CHECK_NOT_NULL(T, copy_nested);
    SOL_CHECK_NOT_NULL(T, copy_file);
    SOL_CHECK_NOT_NULL(T, move_file);
    if (!src_dir || !nested || !file || !copy_dir || !copy_nested ||
        !copy_file || !move_file) {
        goto cleanup;
    }

    SOL_CHECK(T, sol_platform_mkdir_p(nested));
    SOL_CHECK(T, write_file_bytes(file, "hello"));
    SOL_CHECK(T, sol_platform_copy_path_recursive(src_dir, copy_dir));

    char buf[32];
    SOL_CHECK(T, read_file_bytes(copy_file, buf, sizeof(buf)));
    SOL_CHECK_STR(T, buf, "hello");

    SOL_CHECK(T, sol_platform_move_path(copy_file, move_file));
    SolPathInfo info;
    SOL_CHECK(T, sol_platform_get_path_info(move_file, &info));
    SOL_CHECK(T, info.is_regular_file);
    SOL_CHECK(T, !sol_platform_get_path_info(copy_file, &info));

    SOL_CHECK(T, sol_platform_remove_path_recursive(root));
    SOL_CHECK(T, !sol_platform_get_path_info(root, &info));

cleanup:
    (void)sol_platform_remove_path_recursive(root);
    free(src_dir);
    free(nested);
    free(file);
    free(copy_dir);
    free(copy_nested);
    free(copy_file);
    free(move_file);
}

int main(void)
{
    SolTestSuite suite;
    sol_suite_init(&suite, "sol_file_tree_tests");
    SOL_RUN(suite, test_root_clear_notifies_and_resets);
    SOL_RUN(suite, test_refresh_does_not_publish_root_event);
    SOL_RUN(suite, test_platform_recursive_file_ops);
    return sol_suite_report(&suite);
}
