// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "test_harness.h"
#include "sol_theme.h"

#include <string.h>

static void count_changes(void *user_data)
{
    size_t *count = (size_t *)user_data;
    (*count)++;
}

static void test_registry_lifecycle(SolTestCtx *T)
{
    SolThemeRegistry *registry = sol_theme_registry_create();
    SOL_CHECK_NOT_NULL(T, registry);
    if (!registry) return;

    size_t changes = 0u;
    sol_theme_set_change_callback(registry, count_changes, &changes);
    char css[] = ".root { background: #010203; }";
    SOL_CHECK(T, sol_theme_register(registry, &(SolThemeDesc){
        .id = "test.dark", .name = "Dark", .css = css,
        .colors = {
            .background_rgb = 0x010203,
            .primary_rgb = 0x123456,
            .accent_rgb = 0xabcdef,
        },
        .has_colors = true,
    }));
    memset(css, 'x', strlen(css));

    SOL_CHECK_EQ_SZ(T, sol_theme_count(registry), 1u);
    SOL_CHECK_STR(T, sol_theme_active_id(registry), "test.dark");
    SOL_CHECK_STR(T, sol_theme_active_css(registry),
                  ".root { background: #010203; }");
    SolThemeColors colors = {0};
    SOL_CHECK(T, sol_theme_active_colors(registry, &colors));
    SOL_CHECK_EQ_INT(T, (int)colors.background_rgb, 0x010203);
    SOL_CHECK_EQ_INT(T, (int)colors.primary_rgb, 0x123456);
    SOL_CHECK_EQ_INT(T, (int)colors.accent_rgb, 0xabcdef);
    SOL_CHECK_EQ_SZ(T, changes, 1u);
    SOL_CHECK(T, !sol_theme_register(registry, &(SolThemeDesc){
        .id = "test.dark", .name = "Duplicate", .css = ".x{}",
    }));

    SOL_CHECK(T, sol_theme_register(registry, &(SolThemeDesc){
        .id = "test.light", .name = "Light", .base_id = "test.dark",
        .css = ".root { color: white; }",
    }));
    SOL_CHECK(T, strstr(sol_theme_css(registry, "test.light"),
                        "background: #010203") != NULL);
    SOL_CHECK(T, strstr(sol_theme_css(registry, "test.light"),
                        "color: white") != NULL);
    SOL_CHECK(T, sol_theme_set_active(registry, "test.light"));
    SOL_CHECK_STR(T, sol_theme_active_id(registry), "test.light");
    SOL_CHECK(T, sol_theme_active_colors(registry, &colors));
    SOL_CHECK_EQ_INT(T, (int)colors.background_rgb, 0x010203);
    SOL_CHECK_EQ_INT(T, (int)colors.primary_rgb, 0x123456);
    SOL_CHECK_EQ_INT(T, (int)colors.accent_rgb, 0xabcdef);
    SOL_CHECK(T, !sol_theme_set_active(registry, "missing"));
    SOL_CHECK(T, sol_theme_unregister(registry, "test.light"));
    SOL_CHECK_STR(T, sol_theme_active_id(registry), "test.dark");
    SOL_CHECK_EQ_SZ(T, sol_theme_count(registry), 1u);

    sol_theme_registry_destroy(registry);
}

static void test_validation(SolTestCtx *T)
{
    SolThemeRegistry *registry = sol_theme_registry_create();
    SOL_CHECK_NOT_NULL(T, registry);
    if (!registry) return;
    SOL_CHECK(T, !sol_theme_register(registry, NULL));
    SOL_CHECK(T, !sol_theme_register(registry, &(SolThemeDesc){0}));
    SOL_CHECK(T, !sol_theme_register(registry, &(SolThemeDesc){
        .id = "", .name = "Empty", .css = ".x{}",
    }));
    SOL_CHECK(T, !sol_theme_active_colors(registry, NULL));
    SOL_CHECK(T, !sol_theme_unregister(registry, "missing"));
    sol_theme_registry_destroy(registry);
}

int main(void)
{
    SolTestSuite suite;
    sol_suite_init(&suite, "sol_theme_tests");
    SOL_RUN(suite, test_registry_lifecycle);
    SOL_RUN(suite, test_validation);
    return sol_suite_report(&suite);
}
