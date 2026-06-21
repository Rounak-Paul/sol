// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_settings.c — Load / save user settings from $HOME/.sol/settings.json.
 *
 * Hand-rolled minimal JSON parser — no external dependencies.
 * Only the subset of JSON required by our schema is handled; all other
 * constructs are skipped gracefully.
 */

#include "sol_settings.h"
#include "sol_config.h"   /* sol_config_path() */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOL_SETTINGS_FILENAME "settings.json"

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

/*
 * Get default settings.
 *
 * Returns a SolSettings struct with default values.
 */
SolSettings sol_settings_defaults(void)
{
    SolSettings s = {
        .ui_scale  = SOL_SETTINGS_UI_SCALE_DEFAULT,
        .bg_opacity = SOL_SETTINGS_BG_OPACITY_DEFAULT,
    };
    s.bg_effect_id[0] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON parser                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
} JP;  /* JSON parser cursor */

/*
 * Skip whitespace in JSON parser.
 *
 * j  Parser cursor.
 */
static void jp_skip_ws(JP *j)
{
    while (*j->p && isspace((unsigned char)*j->p)) ++j->p;
}

/*
 * Expect and consume a character in JSON after skipping whitespace.
 *
 * j  Parser cursor.
 * c  Character to expect.
 * Returns false if character not found; true on success.
 */
static bool jp_expect(JP *j, char c)
{
    jp_skip_ws(j);
    if (*j->p != c) return false;
    ++j->p;
    return true;
}

/*
 * Parse a JSON string value.
 *
 * j      Parser cursor.
 * buf    Buffer to store string content.
 * bufsz  Buffer size (including null terminator).
 * Returns false if no quoted string found; true on success.
 */
static bool jp_string(JP *j, char *buf, size_t bufsz)
{
    jp_skip_ws(j);
    if (*j->p != '"') return false;
    ++j->p;
    size_t i = 0;
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\') { ++j->p; }   /* skip escape */
        if (i + 1 < bufsz) buf[i++] = *j->p;
        ++j->p;
    }
    if (*j->p == '"') ++j->p;
    buf[i] = '\0';
    return true;
}

/*
 * Parse a JSON number value.
 *
 * j  Parser cursor.
 * Returns parsed float, or NAN if invalid or no number found.
 */
static float jp_float(JP *j)
{
    jp_skip_ws(j);
    const char *start = j->p;
    if (*j->p == '-' || *j->p == '+') ++j->p;
    while (isdigit((unsigned char)*j->p)) ++j->p;
    if (*j->p == '.') {
        ++j->p;
        while (isdigit((unsigned char)*j->p)) ++j->p;
    }
    if (*j->p == 'e' || *j->p == 'E') {
        ++j->p;
        if (*j->p == '-' || *j->p == '+') ++j->p;
        while (isdigit((unsigned char)*j->p)) ++j->p;
    }
    if (j->p == start) return (float)NAN;
    char tmp[64];
    size_t len = (size_t)(j->p - start);
    if (len >= sizeof(tmp)) return (float)NAN;
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    return (float)strtod(tmp, NULL);
}

/*
 * Skip any JSON value (string, number, bool, null, object, array).
 *
 * j  Parser cursor.
 */
static void jp_skip_value(JP *j)
{
    jp_skip_ws(j);
    if (!*j->p) return;
    if (*j->p == '"') {
        char tmp[512];
        jp_string(j, tmp, sizeof(tmp));
        return;
    }
    if (*j->p == '{') {
        ++j->p;
        while (*j->p) {
            jp_skip_ws(j);
            if (*j->p == '}') { ++j->p; return; }
            char key[128]; jp_string(j, key, sizeof(key));
            jp_expect(j, ':');
            jp_skip_value(j);
            jp_skip_ws(j);
            if (*j->p == ',') ++j->p;
        }
        return;
    }
    if (*j->p == '[') {
        ++j->p;
        while (*j->p) {
            jp_skip_ws(j);
            if (*j->p == ']') { ++j->p; return; }
            jp_skip_value(j);
            jp_skip_ws(j);
            if (*j->p == ',') ++j->p;
        }
        return;
    }
    /* number / bool / null: advance past the bare token */
    while (*j->p && !isspace((unsigned char)*j->p) &&
           *j->p != ',' && *j->p != '}' && *j->p != ']')
        ++j->p;
}

/*
 * Parse theme object from JSON.
 *
 * j  Parser cursor positioned at theme object.
 * s  Settings struct to populate.
 */
static void jp_parse_theme(JP *j, SolSettings *s)
{
    if (!jp_expect(j, '{')) return;
    while (*j->p) {
        jp_skip_ws(j);
        if (*j->p == '}') { ++j->p; break; }
        char key[64];
        if (!jp_string(j, key, sizeof(key))) break;
        if (!jp_expect(j, ':')) break;
        if (strcmp(key, "scale") == 0) {
            float v = jp_float(j);
            if (!isnan(v) &&
                v >= SOL_SETTINGS_UI_SCALE_MIN &&
                v <= SOL_SETTINGS_UI_SCALE_MAX)
                s->ui_scale = v;
        } else if (strcmp(key, "effect") == 0) {
            jp_string(j, s->bg_effect_id, sizeof(s->bg_effect_id));
        } else if (strcmp(key, "opacity") == 0) {
            float v = jp_float(j);
            if (!isnan(v) &&
                v >= SOL_SETTINGS_BG_OPACITY_MIN &&
                v <= SOL_SETTINGS_BG_OPACITY_MAX)
                s->bg_opacity = v;
        } else {
            jp_skip_value(j);
        }
        jp_skip_ws(j);
        if (*j->p == ',') ++j->p;
    }
}

/*
 * Parse top-level settings object from JSON.
 *
 * j  Parser cursor positioned at root object.
 * s  Settings struct to populate.
 */
static void jp_parse_root(JP *j, SolSettings *s)
{
    if (!jp_expect(j, '{')) return;
    while (*j->p) {
        jp_skip_ws(j);
        if (*j->p == '}') break;
        char key[64];
        if (!jp_string(j, key, sizeof(key))) break;
        if (!jp_expect(j, ':')) break;
        if (strcmp(key, "theme") == 0) {
            jp_parse_theme(j, s);
        } else {
            jp_skip_value(j);
        }
        jp_skip_ws(j);
        if (*j->p == ',') ++j->p;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Load settings from disk.
 *
 * out  Pointer to SolSettings to populate.
 * Returns true if loaded (or defaults used); false on critical error.
 */
bool sol_settings_load(SolSettings *out)
{
    if (!out) return false;
    *out = sol_settings_defaults();

    char *path = sol_config_path(SOL_SETTINGS_FILENAME);
    if (!path) return false;

    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp) return false;   /* file absent — defaults are fine */

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    bool ok = false;
    if (size > 0 && size < 65536) {
        char *buf = (char *)malloc((size_t)size + 1);
        if (buf) {
            if ((long)fread(buf, 1, (size_t)size, fp) == size) {
                buf[size] = '\0';
                JP j = { .p = buf };
                jp_parse_root(&j, out);
                ok = true;
            }
            free(buf);
        }
    }

    fclose(fp);
    return ok;
}

/*
 * Save settings to disk.
 *
 * settings  Settings to save.
 * Returns false on write error or invalid arguments; true on success.
 */
bool sol_settings_save(const SolSettings *settings)
{
    if (!settings) return false;

    char *path = sol_config_path(SOL_SETTINGS_FILENAME);
    if (!path) return false;

    FILE *fp = fopen(path, "wb");
    free(path);
    if (!fp) return false;

    /* Escape quotes in the effect id for JSON safety (effect ids are
       expected to be simple dotted identifiers, but guard anyway). */
    char esc_id[sizeof(settings->bg_effect_id) * 2 + 1];
    {
        const char *src = settings->bg_effect_id;
        char       *dst = esc_id;
        while (*src) {
            if (*src == '"' || *src == '\\') *dst++ = '\\';
            *dst++ = *src++;
        }
        *dst = '\0';
    }

    int n = fprintf(fp,
        "{\n"
        "  \"theme\": {\n"
        "    \"scale\": %.2f,\n"
        "    \"effect\": \"%s\",\n"
        "    \"opacity\": %.2f\n"
        "  }\n"
        "}\n",
        (double)settings->ui_scale,
        esc_id,
        (double)settings->bg_opacity);

    fclose(fp);
    return n > 0;
}
