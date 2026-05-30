// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_command_flow.c — Unit tests for the command-flow subsystem.
 *
 * Tests the pure logic of:
 *   - sol_ui_normalize_flow_key        (key code normalisation)
 *   - sol_ui_format_key_name           (single-key display strings)
 *   - sol_ui_format_modified_key       (modifier + key display)
 *   - sol_ui_is_modifier_key           (modifier classification)
 *   - sol_ui_flow_matches_prefix       (binding prefix matching)
 *   - sol_ui_system_register_command_flow
 *   - sol_ui_collect_suggestions       (suggestion gathering)
 *
 * No rendering takes place.  SolUISystem is allocated zeroed via
 * calloc and fields are filled manually — signals are NULL which is
 * safe because sol_ui_bump_u32 is NULL-guarded.
 */

#include "test_harness.h"

/* Private header so we can inspect SolUISystem fields directly. */
#include "sol_ui_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Allocate a zeroed SolUISystem; signals are all NULL.
 * sol_ui_bump_u32(NULL) is a no-op, so registration and suggestion
 * gathering work without a live causality instance. */
static SolUISystem *make_ui(void)
{
    return (SolUISystem *)calloc(1, sizeof(SolUISystem));
}

static void free_ui(SolUISystem *ui)
{
    free(ui);
}

/* Register a simple one-key flow. */
static bool reg1(SolUISystem *ui, const char *action, const char *label, SolKeyCode key)
{
    return sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = action,
        .label  = label,
        .key    = key,
    });
}

/* Register a multi-key flow sequence. */
static bool regN(SolUISystem *ui, const char *action, const char *label,
                 const SolKeyCode *seq, const SolModifierMask *mods, size_t len)
{
    return sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action           = action,
        .label            = label,
        .sequence         = seq,
        .step_modifiers   = mods,
        .sequence_length  = len,
    });
}

/* ------------------------------------------------------------------ */
/* Key normalisation                                                   */
/* ------------------------------------------------------------------ */

static void test_normalize_lowercase(SolTestCtx *T)
{
    /* Lowercase letters are normalised to uppercase. */
    for (SolKeyCode k = 'a'; k <= 'z'; ++k) {
        SolKeyCode n = sol_ui_normalize_flow_key(k);
        SOL_CHECK_MSG(T, n == (SolKeyCode)(k - ('a' - 'A')),
                      "normalize('%c') = %u", (char)k, n);
    }
}

static void test_normalize_uppercase_unchanged(SolTestCtx *T)
{
    for (SolKeyCode k = 'A'; k <= 'Z'; ++k) {
        SOL_CHECK_MSG(T, sol_ui_normalize_flow_key(k) == k,
                      "normalize('%c') changed", (char)k);
    }
}

static void test_normalize_special_unchanged(SolTestCtx *T)
{
    SolKeyCode keys[] = {
        SOL_KEY_ENTER, SOL_KEY_TAB, SOL_KEY_BACKSPACE, SOL_KEY_ESCAPE,
        '0', '9', ' ', ',', '.', ';',
    };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        SOL_CHECK_MSG(T, sol_ui_normalize_flow_key(keys[i]) == keys[i],
                      "normalize(K%u) changed", (unsigned)keys[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Key name formatting                                                 */
/* ------------------------------------------------------------------ */

static void test_format_key_letters(SolTestCtx *T)
{
    /* Normalised letters (stored A-Z) display as lowercase. */
    char out[16];
    sol_ui_format_key_name('A', out, sizeof(out));
    SOL_CHECK_STR(T, out, "a");
    sol_ui_format_key_name('Z', out, sizeof(out));
    SOL_CHECK_STR(T, out, "z");
    sol_ui_format_key_name('M', out, sizeof(out));
    SOL_CHECK_STR(T, out, "m");
}

static void test_format_key_special(SolTestCtx *T)
{
    char out[16];
    sol_ui_format_key_name(SOL_KEY_ESCAPE, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Esc");
    sol_ui_format_key_name(SOL_KEY_LEFT_CTRL, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Ctrl");
    sol_ui_format_key_name(SOL_KEY_LEFT_SHIFT, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Shift");
    sol_ui_format_key_name(SOL_KEY_LEFT_ALT, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Alt");
    sol_ui_format_key_name(SOL_KEY_LEFT_SUPER, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Super");
    /* Right modifier aliases produce the same strings. */
    sol_ui_format_key_name(SOL_KEY_RIGHT_CTRL, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Ctrl");
    sol_ui_format_key_name(SOL_KEY_RIGHT_SHIFT, out, sizeof(out));
    SOL_CHECK_STR(T, out, "Shift");
}

static void test_format_key_printable(SolTestCtx *T)
{
    char out[16];
    sol_ui_format_key_name('0', out, sizeof(out));
    SOL_CHECK_STR(T, out, "0");
    sol_ui_format_key_name(',', out, sizeof(out));
    SOL_CHECK_STR(T, out, ",");
    sol_ui_format_key_name(' ', out, sizeof(out));
    SOL_CHECK_STR(T, out, " ");
}

static void test_format_key_unknown(SolTestCtx *T)
{
    /* Non-printable out-of-range keys emit "K<decimal>". */
    char out[32];
    sol_ui_format_key_name(SOL_KEY_ENTER, out, sizeof(out));
    /* SOL_KEY_ENTER = 257, not printable → "K257" */
    SOL_CHECK_STR(T, out, "K257");
}

static void test_format_key_null_out(SolTestCtx *T)
{
    /* Must not crash. */
    sol_ui_format_key_name('A', NULL, 0);
    sol_ui_format_key_name('A', NULL, 16);
    (void)T;
}

/* ------------------------------------------------------------------ */
/* Modified key formatting                                             */
/* ------------------------------------------------------------------ */

static void test_format_modified_no_mods(SolTestCtx *T)
{
    char out[32];
    /* No modifiers — same as plain key name. */
    sol_ui_format_modified_key(SOL_MOD_NONE, 'N', out, sizeof(out));
    SOL_CHECK_STR(T, out, "n");
}

static void test_format_modified_ctrl(SolTestCtx *T)
{
    char out[32];
    sol_ui_format_modified_key(SOL_MOD_CTRL, 'N', out, sizeof(out));
    SOL_CHECK_STR(T, out, "Ctrl+n");
}

static void test_format_modified_ctrl_shift(SolTestCtx *T)
{
    /* Ctrl+Shift+N → "Ctrl+N" (Shift absorbed into uppercase letter). */
    char out[32];
    sol_ui_format_modified_key(SOL_MOD_CTRL | SOL_MOD_SHIFT, 'N', out, sizeof(out));
    SOL_CHECK_STR(T, out, "Ctrl+N");
}

static void test_format_modified_shift_only(SolTestCtx *T)
{
    /* Shift+N → "N" (shift absorbed). */
    char out[32];
    sol_ui_format_modified_key(SOL_MOD_SHIFT, 'N', out, sizeof(out));
    SOL_CHECK_STR(T, out, "N");
}

static void test_format_modified_alt(SolTestCtx *T)
{
    char out[32];
    sol_ui_format_modified_key(SOL_MOD_ALT, 'P', out, sizeof(out));
    SOL_CHECK_STR(T, out, "Alt+p");
}

static void test_format_modified_super(SolTestCtx *T)
{
    char out[32];
    sol_ui_format_modified_key(SOL_MOD_SUPER, 'Q', out, sizeof(out));
    SOL_CHECK_STR(T, out, "Super+q");
}

static void test_format_modified_all(SolTestCtx *T)
{
    /* All four modifiers: Ctrl+Shift+Alt+Super+N → "Ctrl+Alt+Super+N"
       (Shift is absorbed into the uppercase N). */
    char out[64];
    sol_ui_format_modified_key(
        SOL_MOD_CTRL | SOL_MOD_SHIFT | SOL_MOD_ALT | SOL_MOD_SUPER,
        'N', out, sizeof(out));
    /* Order: Ctrl Alt Super (Shift absorbed), key = N */
    SOL_CHECK_STR(T, out, "Ctrl+Alt+Super+N");
}

/* ------------------------------------------------------------------ */
/* Modifier key classification                                         */
/* ------------------------------------------------------------------ */

static void test_is_modifier_key(SolTestCtx *T)
{
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_LEFT_CTRL));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_RIGHT_CTRL));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_LEFT_SHIFT));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_RIGHT_SHIFT));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_LEFT_ALT));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_RIGHT_ALT));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_LEFT_SUPER));
    SOL_CHECK(T,  sol_ui_is_modifier_key(SOL_KEY_RIGHT_SUPER));
    SOL_CHECK(T, !sol_ui_is_modifier_key('A'));
    SOL_CHECK(T, !sol_ui_is_modifier_key(SOL_KEY_ENTER));
    SOL_CHECK(T, !sol_ui_is_modifier_key(SOL_KEY_ESCAPE));
}

/* ------------------------------------------------------------------ */
/* Flow registration                                                   */
/* ------------------------------------------------------------------ */

static void test_register_single_key(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    bool ok = reg1(ui, "editor.save", "Save", 'S');
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, ui->command_flow_count, 1);
    SOL_CHECK_STR(T, ui->command_flows[0].action, "editor.save");
    SOL_CHECK_STR(T, ui->command_flows[0].label,  "Save");
    SOL_CHECK_EQ_INT(T, (int)ui->command_flows[0].sequence[0], 'S');
    SOL_CHECK_EQ_SZ(T, ui->command_flows[0].sequence_length, 1);
    free_ui(ui);
}

static void test_register_multi_key(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    SolKeyCode        seq[]  = { 'W', 'V' };
    SolModifierMask   mods[] = { SOL_MOD_NONE, SOL_MOD_NONE };
    bool ok = regN(ui, "buffer.split.vertical", "Split Vertical", seq, mods, 2);
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, ui->command_flows[0].sequence_length, 2);
    SOL_CHECK_EQ_INT(T, (int)ui->command_flows[0].sequence[0], 'W');
    SOL_CHECK_EQ_INT(T, (int)ui->command_flows[0].sequence[1], 'V');
    free_ui(ui);
}

static void test_register_update_existing(SolTestCtx *T)
{
    /* Re-registering the same action should update (not duplicate). */
    SolUISystem *ui = make_ui();
    reg1(ui, "editor.save", "Save", 'S');
    reg1(ui, "editor.save", "Save File", 'S');  /* update */
    SOL_CHECK_EQ_SZ(T, ui->command_flow_count, 1);
    SOL_CHECK_STR(T, ui->command_flows[0].label, "Save File");
    free_ui(ui);
}

static void test_register_null_action(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    bool ok = sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = NULL,
        .key    = 'A',
    });
    SOL_CHECK(T, !ok);
    SOL_CHECK_EQ_SZ(T, ui->command_flow_count, 0);
    free_ui(ui);
}

static void test_register_zero_sequence(SolTestCtx *T)
{
    /* Sequence length 0 with no .key set — must reject. */
    SolUISystem *ui = make_ui();
    bool ok = sol_ui_system_register_command_flow(ui, &(SolCommandFlowDesc){
        .action = "noop", .key = SOL_KEY_UNKNOWN, .sequence = NULL, .sequence_length = 0,
    });
    SOL_CHECK(T, !ok);
    free_ui(ui);
}

static void test_register_capacity(SolTestCtx *T)
{
    /* Fill up to SOL_UI_MAX_COMMAND_FLOWS - verify no overflow. */
    SolUISystem *ui = make_ui();
    char action[64];
    for (size_t i = 0; i < SOL_UI_MAX_COMMAND_FLOWS; ++i) {
        snprintf(action, sizeof(action), "action.%zu", i);
        bool ok = reg1(ui, action, "X", (SolKeyCode)('A' + (i % 26)));
        SOL_CHECK_MSG(T, ok || i >= SOL_UI_MAX_COMMAND_FLOWS,
                      "registration %zu failed unexpectedly", i);
    }
    SOL_CHECK_EQ_SZ(T, ui->command_flow_count, SOL_UI_MAX_COMMAND_FLOWS);
    free_ui(ui);
}

/* ------------------------------------------------------------------ */
/* Prefix matching                                                     */
/* ------------------------------------------------------------------ */

static void test_matches_prefix_empty(SolTestCtx *T)
{
    /* Empty prefix matches any binding. */
    SolCommandFlowBinding flow = {0};
    flow.sequence[0] = 'W';
    flow.sequence[1] = 'V';
    flow.sequence_length = 2;

    SOL_CHECK(T, sol_ui_flow_matches_prefix(&flow, NULL, NULL, 0));
}

static void test_matches_prefix_exact(SolTestCtx *T)
{
    SolCommandFlowBinding flow = {0};
    flow.sequence[0] = 'W';
    flow.sequence[1] = 'V';
    flow.sequence_length = 2;

    SolKeyCode prefix[2] = { 'W', 'V' };
    SOL_CHECK(T, sol_ui_flow_matches_prefix(&flow, prefix, NULL, 2));
}

static void test_matches_prefix_mismatch(SolTestCtx *T)
{
    SolCommandFlowBinding flow = {0};
    flow.sequence[0] = 'W';
    flow.sequence[1] = 'V';
    flow.sequence_length = 2;

    SolKeyCode bad[1] = { 'X' };
    SOL_CHECK(T, !sol_ui_flow_matches_prefix(&flow, bad, NULL, 1));
}

static void test_matches_prefix_too_long(SolTestCtx *T)
{
    /* Prefix longer than binding length → no match. */
    SolCommandFlowBinding flow = {0};
    flow.sequence[0] = 'W';
    flow.sequence_length = 1;

    SolKeyCode prefix[2] = { 'W', 'V' };
    SOL_CHECK(T, !sol_ui_flow_matches_prefix(&flow, prefix, NULL, 2));
}

static void test_matches_prefix_with_mods(SolTestCtx *T)
{
    SolCommandFlowBinding flow = {0};
    flow.sequence[0]       = 'S';
    flow.step_modifiers[0] = SOL_MOD_CTRL;
    flow.sequence_length = 1;

    SolKeyCode        prefix[1]    = { 'S' };
    SolModifierMask   mods_ok[1]   = { SOL_MOD_CTRL };
    SolModifierMask   mods_bad[1]  = { SOL_MOD_NONE };

    SOL_CHECK(T,  sol_ui_flow_matches_prefix(&flow, prefix, mods_ok,  1));
    SOL_CHECK(T, !sol_ui_flow_matches_prefix(&flow, prefix, mods_bad, 1));
}

/* ------------------------------------------------------------------ */
/* Suggestion collection                                               */
/* ------------------------------------------------------------------ */

static void test_suggestions_empty_system(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    SolFlowSuggestion sugs[8];
    size_t n = sol_ui_collect_suggestions(ui, sugs, 8);
    SOL_CHECK_EQ_SZ(T, n, 0);
    free_ui(ui);
}

static void test_suggestions_top_level(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    reg1(ui, "act.a", "A", 'A');
    reg1(ui, "act.b", "B", 'B');
    reg1(ui, "act.c", "C", 'C');

    /* Empty prefix → all 3 top-level keys. */
    SolFlowSuggestion sugs[8];
    size_t n = sol_ui_collect_suggestions(ui, sugs, 8);
    SOL_CHECK_EQ_SZ(T, n, 3);
    free_ui(ui);
}

static void test_suggestions_prefix_filters(SolTestCtx *T)
{
    SolUISystem *ui = make_ui();
    SolKeyCode   wa[]  = { 'W', 'A' };
    SolKeyCode   wb[]  = { 'W', 'B' };
    SolKeyCode   xc[]  = { 'X', 'C' };
    SolModifierMask z[] = { 0, 0 };

    regN(ui, "act.wa", "WA", wa, z, 2);
    regN(ui, "act.wb", "WB", wb, z, 2);
    regN(ui, "act.xc", "XC", xc, z, 2);

    /* Empty prefix: 2 top-level keys ('W' and 'X'). */
    SolFlowSuggestion sugs[8];
    size_t n = sol_ui_collect_suggestions(ui, sugs, 8);
    SOL_CHECK_EQ_SZ(T, n, 2);

    /* Set prefix to ['W'] → only suggestions with W prefix. */
    ui->leader_prefix[0]           = 'W';
    ui->leader_prefix_modifiers[0] = SOL_MOD_NONE;
    ui->leader_prefix_length       = 1;

    n = sol_ui_collect_suggestions(ui, sugs, 8);
    SOL_CHECK_EQ_SZ(T, n, 2);  /* 'A' and 'B' continuations */
    /* Both match W prefix. */
    bool saw_a = false, saw_b = false;
    for (size_t i = 0; i < n; ++i) {
        if (sugs[i].key == 'A') saw_a = true;
        if (sugs[i].key == 'B') saw_b = true;
    }
    SOL_CHECK(T, saw_a);
    SOL_CHECK(T, saw_b);

    free_ui(ui);
}

static void test_suggestions_dedup(SolTestCtx *T)
{
    /* Multiple bindings sharing the same next key should produce only one
       suggestion entry (the deduplication path). */
    SolUISystem *ui = make_ui();
    SolKeyCode   seq1[] = { 'W', 'V', 'H' };  /* W → V → H */
    SolKeyCode   seq2[] = { 'W', 'V', 'S' };  /* W → V → S */
    SolModifierMask z[] = { 0, 0, 0 };

    regN(ui, "act.wvh", "WVH", seq1, z, 3);
    regN(ui, "act.wvs", "WVS", seq2, z, 3);

    /* Top level: only 'W'. */
    SolFlowSuggestion sugs[8];
    size_t n = sol_ui_collect_suggestions(ui, sugs, 8);
    SOL_CHECK_EQ_SZ(T, n, 1);
    SOL_CHECK_EQ_INT(T, (int)sugs[0].key, 'W');
    /* continuation_count for 'W' should reflect that V leads to 2 more. */
    SOL_CHECK_EQ_INT(T, (int)sugs[0].continuation_count, 1);

    free_ui(ui);
}

static void test_suggestions_capacity_cap(SolTestCtx *T)
{
    /* When capacity is smaller than the number of suggestions, we get
       exactly capacity entries, no overflow. */
    SolUISystem *ui = make_ui();
    for (int i = 0; i < 10; ++i) {
        char action[32];
        snprintf(action, sizeof(action), "act.%d", i);
        reg1(ui, action, "X", (SolKeyCode)('A' + i));
    }
    SolFlowSuggestion sugs[4];
    size_t n = sol_ui_collect_suggestions(ui, sugs, 4);
    SOL_CHECK_EQ_SZ(T, n, 4);
    free_ui(ui);
}

static void test_suggestions_null_safety(SolTestCtx *T)
{
    SolFlowSuggestion sugs[4];
    SOL_CHECK_EQ_SZ(T, sol_ui_collect_suggestions(NULL, sugs, 4), 0);
    SolUISystem *ui = make_ui();
    SOL_CHECK_EQ_SZ(T, sol_ui_collect_suggestions(ui, NULL, 4), 0);
    SOL_CHECK_EQ_SZ(T, sol_ui_collect_suggestions(ui, sugs, 0), 0);
    free_ui(ui);
}

/* ------------------------------------------------------------------ */
/* Performance benchmark                                               */
/* ------------------------------------------------------------------ */

typedef struct BenchFlowCtx { SolUISystem *ui; SolFlowSuggestion sugs[32]; } BenchFlowCtx;

static void bench_collect_fn(void *ud)
{
    BenchFlowCtx *c = (BenchFlowCtx *)ud;
    sol_ui_collect_suggestions(c->ui, c->sugs, 32);
}

static void run_benchmarks(void)
{
    SolUISystem *ui = make_ui();
    /* Register 32 two-step flows. */
    for (int i = 0; i < 32; ++i) {
        char action[32];
        snprintf(action, sizeof(action), "perf.%d", i);
        SolKeyCode seq[] = { (SolKeyCode)('A' + (i / 10)), (SolKeyCode)('A' + (i % 26)) };
        SolModifierMask mods[] = { 0, 0 };
        regN(ui, action, "X", seq, mods, 2);
    }
    BenchFlowCtx ctx = { .ui = ui };
    sol_bench("collect_suggestions x32", 10000, bench_collect_fn, &ctx);
    free_ui(ui);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_command_flow");

    /* Key normalisation */
    SOL_RUN(s, test_normalize_lowercase);
    SOL_RUN(s, test_normalize_uppercase_unchanged);
    SOL_RUN(s, test_normalize_special_unchanged);

    /* Key name formatting */
    SOL_RUN(s, test_format_key_letters);
    SOL_RUN(s, test_format_key_special);
    SOL_RUN(s, test_format_key_printable);
    SOL_RUN(s, test_format_key_unknown);
    SOL_RUN(s, test_format_key_null_out);

    /* Modified key formatting */
    SOL_RUN(s, test_format_modified_no_mods);
    SOL_RUN(s, test_format_modified_ctrl);
    SOL_RUN(s, test_format_modified_ctrl_shift);
    SOL_RUN(s, test_format_modified_shift_only);
    SOL_RUN(s, test_format_modified_alt);
    SOL_RUN(s, test_format_modified_super);
    SOL_RUN(s, test_format_modified_all);

    /* Modifier classification */
    SOL_RUN(s, test_is_modifier_key);

    /* Registration */
    SOL_RUN(s, test_register_single_key);
    SOL_RUN(s, test_register_multi_key);
    SOL_RUN(s, test_register_update_existing);
    SOL_RUN(s, test_register_null_action);
    SOL_RUN(s, test_register_zero_sequence);
    SOL_RUN(s, test_register_capacity);

    /* Prefix matching */
    SOL_RUN(s, test_matches_prefix_empty);
    SOL_RUN(s, test_matches_prefix_exact);
    SOL_RUN(s, test_matches_prefix_mismatch);
    SOL_RUN(s, test_matches_prefix_too_long);
    SOL_RUN(s, test_matches_prefix_with_mods);

    /* Suggestion collection */
    SOL_RUN(s, test_suggestions_empty_system);
    SOL_RUN(s, test_suggestions_top_level);
    SOL_RUN(s, test_suggestions_prefix_filters);
    SOL_RUN(s, test_suggestions_dedup);
    SOL_RUN(s, test_suggestions_capacity_cap);
    SOL_RUN(s, test_suggestions_null_safety);

    run_benchmarks();

    return sol_suite_report(&s);
}
