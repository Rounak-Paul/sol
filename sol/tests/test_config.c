// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_config.c — bindings.conf editing and settings.json preferences.
 *
 * Covers:
 *   - sol_config_parse_chord / sol_config_format_chord round trips and
 *     rejection of malformed chords
 *   - sol_config_default_bindings
 *   - sol_config_save_binding / save_leader / reset_bindings: surgical,
 *     comment-preserving rewrites that the loader then honours, including
 *     the `unbind` directive
 *   - sol_ui_system_reset_keymap
 *   - settings.json round trip of the Preferences fields, and tolerance of
 *     mistyped values
 *
 * Every test runs against a throwaway config home (HOME / APPDATA pointed
 * at a temp directory), never the developer's real ~/.sol.
 */

#include "test_harness.h"

#include "sol_config.h"
#include "sol_platform.h"
#include "sol_settings.h"
#include "sol_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char g_home[256];

/* Point the config home at a fresh temp directory, removing the last one. */
static bool use_temp_home(void)
{
    if (g_home[0] != '\0') (void)sol_platform_remove_path_recursive(g_home);
    snprintf(g_home, sizeof(g_home), "/tmp/sol_config_home_%llu",
             (unsigned long long)sol_test_now_ns());
    if (!sol_platform_mkdir_p(g_home)) return false;
#if defined(_WIN32)
    return _putenv_s("APPDATA", g_home) == 0;
#else
    return setenv("HOME", g_home, 1) == 0;
#endif
}

/* Read a file inside the config dir into a heap string (NULL if absent). */
static char *read_config_file(const char *name)
{
    char *path = sol_config_path(name);
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = size >= 0 ? (char *)malloc((size_t)size + 1u) : NULL;
    if (buf) {
        const size_t n = fread(buf, 1u, (size_t)size, fp);
        buf[n] = '\0';
    }
    fclose(fp);
    return buf;
}

/* Overwrite a file inside the config dir. */
static bool write_config_file(const char *name, const char *text)
{
    char *path = sol_config_path(name);
    if (!path) return false;
    FILE *fp = fopen(path, "wb");
    free(path);
    if (!fp) return false;
    const size_t len = strlen(text);
    const bool ok = fwrite(text, 1u, len, fp) == len;
    return (fclose(fp) == 0) && ok;
}

/* Count non-overlapping occurrences of needle in haystack. */
static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const size_t len = strlen(needle);
    for (const char *p = haystack; p && (p = strstr(p, needle)) != NULL; p += len) ++n;
    return n;
}

/* Registry flow for action, or NULL. */
static const SolCommandFlowBinding *find_flow(const SolUISystem *ui, const char *action)
{
    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        if (strcmp(ui->command_flows[i].action, action) == 0) return &ui->command_flows[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Chord text                                                          */
/* ------------------------------------------------------------------ */

static void test_chord_round_trip(SolTestCtx *T)
{
    SolKeyCode      seq[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    SolModifierMask mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t          len = 0u;
    char            text[64];

    SOL_CHECK(T, sol_config_parse_chord("b shift+s", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK_EQ_SZ(T, len, 2u);
    SOL_CHECK_EQ_INT(T, (int)seq[0], 'B');
    SOL_CHECK_EQ_INT(T, (int)seq[1], 'S');
    SOL_CHECK_EQ_INT(T, (int)mods[0], (int)SOL_MOD_NONE);
    SOL_CHECK_EQ_INT(T, (int)mods[1], (int)SOL_MOD_SHIFT);
    SOL_CHECK(T, sol_config_format_chord(seq, mods, len, text, sizeof(text)));
    SOL_CHECK_STR(T, text, "b shift+s");

    SOL_CHECK(T, sol_config_parse_chord("  E  W   Backspace ", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, sol_config_format_chord(seq, mods, len, text, sizeof(text)));
    SOL_CHECK_STR(T, text, "e w backspace");

    SOL_CHECK(T, sol_config_parse_chord("alt+super+space", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, sol_config_format_chord(seq, mods, len, text, sizeof(text)));
    SOL_CHECK_STR(T, text, "alt+super+space");
}

static void test_chord_rejects_malformed(SolTestCtx *T)
{
    SolKeyCode      seq[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    SolModifierMask mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t          len = 0u;

    SOL_CHECK(T, !sol_config_parse_chord("", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("   ", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("b #", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("shift+", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("bogus", SOL_MOD_CTRL, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("ctrl+b", SOL_MOD_ALT, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("alt+b", SOL_MOD_ALT, seq, mods, &len));
    SOL_CHECK(T, !sol_config_parse_chord("a b c d e f g h i", SOL_MOD_CTRL, seq, mods, &len));
}

static void test_default_bindings_table(SolTestCtx *T)
{
    SolConfigBinding table[128];
    const size_t n = sol_config_default_bindings(table, 128u);
    SOL_CHECK(T, n >= 40u && n <= 128u);
    SOL_CHECK_EQ_SZ(T, sol_config_default_bindings(NULL, 0u), n);

    bool saw_save_all = false;
    for (size_t i = 0u; i < n; ++i) {
        if (strcmp(table[i].action, "buffer.save_all") == 0) {
            saw_save_all = true;
            SOL_CHECK_STR(T, table[i].chord, "b shift+s");
        }
    }
    SOL_CHECK(T, saw_save_all);
}

/* ------------------------------------------------------------------ */
/* bindings.conf editing                                               */
/* ------------------------------------------------------------------ */

static void test_save_binding_is_surgical(SolTestCtx *T)
{
    SOL_CHECK(T, use_temp_home());
    SOL_CHECK(T, write_config_file("bindings.conf",
        "# my keymap\n"
        "leader ctrl\n"
        "bind L b c   buffer.new   # trailing note\n"
        "bind L b c   buffer.new\n"
        "bind L q q   custom.thing\n"));

    const SolKeyCode      seq[]  = { 'Z', 'X' };
    const SolModifierMask mods[] = { SOL_MOD_NONE, SOL_MOD_SHIFT };
    SOL_CHECK(T, sol_config_save_binding("buffer.new", seq, mods, 2u));

    char *text = read_config_file("bindings.conf");
    SOL_CHECK_NOT_NULL(T, text);
    if (text) {
        SOL_CHECK(T, strstr(text, "# my keymap\n") == text);
        SOL_CHECK_EQ_INT(T, count_occurrences(text, "buffer.new"), 1);
        SOL_CHECK(T, strstr(text, "bind L z shift+x") != NULL);
        SOL_CHECK(T, strstr(text, "bind L q q   custom.thing\n") != NULL);
        SOL_CHECK(T, strstr(text, "trailing note") == NULL);
        free(text);
    }

    SOL_CHECK(T, sol_config_save_binding("new.action", seq, NULL, 1u));
    text = read_config_file("bindings.conf");
    SOL_CHECK(T, text && strstr(text, "custom.thing\nbind L z") != NULL);
    free(text);

    SOL_CHECK(T, !sol_config_save_binding("bad action", seq, NULL, 1u));
    SOL_CHECK(T, !sol_config_save_binding("", seq, NULL, 1u));
}

static void test_unbind_and_reload(SolTestCtx *T)
{
    SOL_CHECK(T, use_temp_home());
    SolUISystem *ui = (SolUISystem *)calloc(1, sizeof(SolUISystem));
    SOL_CHECK_NOT_NULL(T, ui);
    if (!ui) return;

    SOL_CHECK(T, sol_config_load_bindings(ui) > 40);
    SOL_CHECK_NOT_NULL(T, find_flow(ui, "buffer.close"));

    SOL_CHECK(T, sol_config_save_binding("buffer.close", NULL, NULL, 0u));
    char *text = read_config_file("bindings.conf");
    SOL_CHECK(T, text && strstr(text, "unbind buffer.close\n") != NULL);
    SOL_CHECK(T, text && strstr(text, "bind L b x") == NULL);
    free(text);

    sol_ui_system_reset_keymap(ui);
    SOL_CHECK_EQ_SZ(T, ui->command_flow_count, 0u);

    /* An owned command (built-in default chord) stays registered but
       unbound after `unbind`. */
    const SolKeyCode builtin_seq[] = { 'B', 'X' };
    SOL_CHECK(T, sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "buffer.close", .sequence = builtin_seq, .sequence_length = 2u,
    }));
    SOL_CHECK(T, sol_config_load_bindings(ui) > 40);
    const SolCommandFlowBinding *closed = find_flow(ui, "buffer.close");
    SOL_CHECK_NOT_NULL(T, closed);
    if (closed) {
        SOL_CHECK_EQ_SZ(T, closed->sequence_length, 0u);
        SOL_CHECK_EQ_SZ(T, closed->default_length, 2u);
    }

    const SolKeyCode seq[] = { 'K' };
    SOL_CHECK(T, sol_config_save_binding("buffer.close", seq, NULL, 1u));
    sol_ui_system_reset_keymap(ui);
    SOL_CHECK(T, sol_config_load_bindings(ui) > 40);
    const SolCommandFlowBinding *flow = find_flow(ui, "buffer.close");
    SOL_CHECK_NOT_NULL(T, flow);
    if (flow) {
        SOL_CHECK_EQ_SZ(T, flow->sequence_length, 1u);
        SOL_CHECK_EQ_INT(T, (int)flow->sequence[0], 'K');
    }
    free(ui);
}

static void test_save_leader_rebases_literal_binds(SolTestCtx *T)
{
    SOL_CHECK(T, use_temp_home());
    SOL_CHECK(T, write_config_file("bindings.conf",
        "# header\n"
        "bind ctrl q x   test.literal\n"
        "leader ctrl\n"
        "bind L q y      test.placeholder\n"));

    SOL_CHECK(T, sol_config_save_leader(SOL_MOD_CTRL, SOL_MOD_ALT));
    char *text = read_config_file("bindings.conf");
    SOL_CHECK_NOT_NULL(T, text);
    if (text) {
        SOL_CHECK_EQ_INT(T, count_occurrences(text, "leader "), 1);
        SOL_CHECK(T, strstr(text, "# header\nleader alt\nbind L q x test.literal\n") == text);
        SOL_CHECK(T, strstr(text, "bind L q y      test.placeholder\n") != NULL);
        free(text);
    }

    SolUISystem *ui = (SolUISystem *)calloc(1, sizeof(SolUISystem));
    SOL_CHECK_NOT_NULL(T, ui);
    if (!ui) return;
    SOL_CHECK_EQ_INT(T, sol_config_load_bindings(ui), 2);
    SOL_CHECK_EQ_INT(T, (int)ui->leader_modifier, (int)SOL_MOD_ALT);

    SOL_CHECK(T, sol_config_reset_bindings());
    sol_ui_system_reset_keymap(ui);
    SOL_CHECK(T, sol_config_load_bindings(ui) > 40);
    SOL_CHECK_EQ_INT(T, (int)ui->leader_modifier, (int)SOL_MOD_CTRL);
    free(ui);
}

/* ------------------------------------------------------------------ */
/* settings.json preferences                                           */
/* ------------------------------------------------------------------ */

static void test_preferences_round_trip(SolTestCtx *T)
{
    SOL_CHECK(T, use_temp_home());
    SolSettings s = sol_settings_defaults();
    SOL_CHECK(T, !s.autosave_enabled);
    SOL_CHECK(T, s.caret_blink);
    SOL_CHECK(T, !s.show_hidden_files);
    SOL_CHECK_EQ_FLOAT(T, s.autosave_delay, SOL_SETTINGS_AUTOSAVE_DELAY_DEFAULT, 1e-6f);

    s.autosave_enabled  = true;
    s.autosave_delay    = 3.25f;
    s.caret_blink       = false;
    s.show_hidden_files = true;
    SOL_CHECK(T, sol_settings_save(&s));

    SolSettings loaded;
    SOL_CHECK(T, sol_settings_load(&loaded));
    SOL_CHECK(T, loaded.autosave_enabled);
    SOL_CHECK_EQ_FLOAT(T, loaded.autosave_delay, 3.25f, 1e-6f);
    SOL_CHECK(T, !loaded.caret_blink);
    SOL_CHECK(T, loaded.show_hidden_files);
}

static void test_preferences_tolerate_bad_values(SolTestCtx *T)
{
    SOL_CHECK(T, use_temp_home());
    SOL_CHECK(T, write_config_file("settings.json",
        "{\n"
        "  \"editor\": {\n"
        "    \"autosave\": \"yes\",\n"
        "    \"autosave_delay\": \"soon\",\n"
        "    \"caret_blink\": false\n"
        "  },\n"
        "  \"explorer\": { \"show_hidden\": 1, \"extra\": [1, {\"a\": 2}] },\n"
        "  \"theme\": { \"scale\": 1.5 }\n"
        "}\n"));

    SolSettings loaded;
    SOL_CHECK(T, sol_settings_load(&loaded));
    SOL_CHECK(T, !loaded.autosave_enabled);
    SOL_CHECK_EQ_FLOAT(T, loaded.autosave_delay, SOL_SETTINGS_AUTOSAVE_DELAY_DEFAULT, 1e-6f);
    SOL_CHECK(T, !loaded.caret_blink);
    SOL_CHECK(T, !loaded.show_hidden_files);
    SOL_CHECK_EQ_FLOAT(T, loaded.ui_scale, 1.5f, 1e-6f);

    SOL_CHECK(T, write_config_file("settings.json",
        "{ \"editor\": { \"autosave_delay\": 99 } }"));
    SOL_CHECK(T, sol_settings_load(&loaded));
    SOL_CHECK_EQ_FLOAT(T, loaded.autosave_delay, SOL_SETTINGS_AUTOSAVE_DELAY_DEFAULT, 1e-6f);
}

int main(void)
{
    SolTestSuite suite;
    sol_suite_init(&suite, "sol_config_tests");
    SOL_RUN(suite, test_chord_round_trip);
    SOL_RUN(suite, test_chord_rejects_malformed);
    SOL_RUN(suite, test_default_bindings_table);
    SOL_RUN(suite, test_save_binding_is_surgical);
    SOL_RUN(suite, test_unbind_and_reload);
    SOL_RUN(suite, test_save_leader_rebases_literal_binds);
    SOL_RUN(suite, test_preferences_round_trip);
    SOL_RUN(suite, test_preferences_tolerate_bad_values);
    if (g_home[0] != '\0') (void)sol_platform_remove_path_recursive(g_home);
    return sol_suite_report(&suite);
}
