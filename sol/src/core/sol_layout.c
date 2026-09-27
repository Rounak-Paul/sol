// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_layout.c — Load / save the workspace arrangement in $HOME/.sol/layout. */

#include "sol_layout.h"
#include "sol_config.h"
#include "sol_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOL_LAYOUT_FILENAME "layout"
#define SOL_LAYOUT_LINE_MAX 128

static const struct {
    SolTerminalPosition position;
    const char         *name;
} SOL_LAYOUT_POSITIONS[] = {
    { SOL_TERMINAL_POSITION_BOTTOM, "bottom" },
    { SOL_TERMINAL_POSITION_RIGHT,  "right"  },
    { SOL_TERMINAL_POSITION_FLOAT,  "float"  },
};

#define SOL_LAYOUT_POSITION_COUNT \
    (sizeof(SOL_LAYOUT_POSITIONS) / sizeof(SOL_LAYOUT_POSITIONS[0]))

/*
 * Clamp value into [min, max], substituting fallback for NaN or infinity.
 *
 * value     Candidate ratio.
 * min, max  Inclusive bounds.
 * fallback  Value used when value is not finite.
 * Returns   The clamped ratio.
 */
static float sol_layout_clamp(float value, float min, float max, float fallback)
{
    if (!isfinite(value)) return fallback;
    return value < min ? min : (value > max ? max : value);
}

/*
 * Parse a strictly numeric token into a float.
 *
 * text     NUL-terminated token.
 * out      Receives the parsed value on success.
 * Returns  true when the whole token is a finite number.
 */
static bool sol_layout_parse_float(const char *text, float *out)
{
    char *end = NULL;
    const float value = strtof(text, &end);
    if (end == text || *end != '\0' || !isfinite(value)) return false;
    *out = value;
    return true;
}

SolLayout sol_layout_defaults(void)
{
    return (SolLayout){
        .tree_ratio        = SOL_LAYOUT_TREE_RATIO_DEFAULT,
        .terminal_position = SOL_TERMINAL_POSITION_BOTTOM,
        .terminal_ratio    = SOL_LAYOUT_TERMINAL_RATIO_DEFAULT,
        .search_ratio      = SOL_LAYOUT_SEARCH_RATIO_DEFAULT,
    };
}

void sol_layout_sanitize(SolLayout *layout)
{
    if (!layout) return;
    layout->tree_ratio = sol_layout_clamp(layout->tree_ratio,
                                          SOL_LAYOUT_TREE_RATIO_MIN,
                                          SOL_LAYOUT_TREE_RATIO_MAX,
                                          SOL_LAYOUT_TREE_RATIO_DEFAULT);
    layout->terminal_ratio = sol_layout_clamp(layout->terminal_ratio,
                                              SOL_LAYOUT_TERMINAL_RATIO_MIN,
                                              SOL_LAYOUT_TERMINAL_RATIO_MAX,
                                              SOL_LAYOUT_TERMINAL_RATIO_DEFAULT);
    layout->search_ratio = sol_layout_clamp(layout->search_ratio,
                                            SOL_LAYOUT_SEARCH_RATIO_MIN,
                                            SOL_LAYOUT_SEARCH_RATIO_MAX,
                                            SOL_LAYOUT_SEARCH_RATIO_DEFAULT);
    bool known = false;
    for (size_t i = 0; i < SOL_LAYOUT_POSITION_COUNT; ++i)
        known = known || SOL_LAYOUT_POSITIONS[i].position == layout->terminal_position;
    if (!known) layout->terminal_position = SOL_TERMINAL_POSITION_BOTTOM;
}

/*
 * Apply one "key value" pair to layout; unknown keys and bad values are ignored.
 *
 * layout  Layout being populated.
 * key     Field name.
 * value   Field value text.
 */
static void sol_layout_apply_pair(SolLayout *layout, const char *key, const char *value)
{
    float number;
    if (strcmp(key, "terminal_position") == 0) {
        for (size_t i = 0; i < SOL_LAYOUT_POSITION_COUNT; ++i) {
            if (strcmp(value, SOL_LAYOUT_POSITIONS[i].name) == 0) {
                layout->terminal_position = SOL_LAYOUT_POSITIONS[i].position;
                return;
            }
        }
    } else if (sol_layout_parse_float(value, &number)) {
        if (strcmp(key, "tree_ratio") == 0)          layout->tree_ratio = number;
        else if (strcmp(key, "terminal_ratio") == 0) layout->terminal_ratio = number;
        else if (strcmp(key, "search_ratio") == 0)   layout->search_ratio = number;
    }
}

bool sol_layout_load(SolLayout *out)
{
    if (!out) return false;
    *out = sol_layout_defaults();
    char *path = sol_config_path(SOL_LAYOUT_FILENAME);
    if (!path) return false;
    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp) return false;

    char line[SOL_LAYOUT_LINE_MAX];
    while (fgets(line, sizeof(line), fp)) {
        const size_t len = strcspn(line, "\r\n");
        if (line[len] == '\0' && !feof(fp)) {
            int c;
            while ((c = fgetc(fp)) != EOF && c != '\n') {}
            continue;
        }
        line[len] = '\0';
        char key[32];
        char value[32];
        char extra;
        if (sscanf(line, " %31s %31s %c", key, value, &extra) == 2 && key[0] != '#')
            sol_layout_apply_pair(out, key, value);
    }
    fclose(fp);
    sol_layout_sanitize(out);
    return true;
}

bool sol_layout_save(const SolLayout *layout)
{
    if (!layout) return false;
    SolLayout clean = *layout;
    sol_layout_sanitize(&clean);

    const char *position = SOL_LAYOUT_POSITIONS[0].name;
    for (size_t i = 0; i < SOL_LAYOUT_POSITION_COUNT; ++i) {
        if (SOL_LAYOUT_POSITIONS[i].position == clean.terminal_position)
            position = SOL_LAYOUT_POSITIONS[i].name;
    }

    char *path = sol_config_path(SOL_LAYOUT_FILENAME);
    if (!path) return false;
    char tmp[4160];
    const int tmp_len = snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path,
                                 sol_platform_process_id());
    if (tmp_len < 0 || (size_t)tmp_len >= sizeof(tmp)) {
        free(path);
        return false;
    }

    bool ok = false;
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        ok = fprintf(fp,
                     "tree_ratio %.4f\n"
                     "terminal_position %s\n"
                     "terminal_ratio %.4f\n"
                     "search_ratio %.4f\n",
                     (double)clean.tree_ratio, position,
                     (double)clean.terminal_ratio,
                     (double)clean.search_ratio) > 0;
        ok = ok && fflush(fp) == 0 && sol_platform_sync_file(fp);
        ok = (fclose(fp) == 0) && ok;
        ok = ok && sol_platform_replace_file(tmp, path);
        if (!ok) remove(tmp);
    }
    free(path);
    return ok;
}
