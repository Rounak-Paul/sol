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
        .ui_scale         = SOL_SETTINGS_UI_SCALE_DEFAULT,
        .bg_opacity       = SOL_SETTINGS_BG_OPACITY_DEFAULT,
        .bg_blur          = SOL_SETTINGS_BG_BLUR_DEFAULT,
        .buffer_blur      = SOL_SETTINGS_BUFFER_BLUR_DEFAULT,
        .corner_radius    = SOL_SETTINGS_CORNER_RADIUS_DEFAULT,
        .panel_blur       = SOL_SETTINGS_PANEL_BLUR_DEFAULT,
        .titlebar_blur    = SOL_SETTINGS_TITLEBAR_BLUR_DEFAULT,
        .panel_opacity    = SOL_SETTINGS_PANEL_OPACITY_DEFAULT,
        .scrollbar_width  = SOL_SETTINGS_SCROLLBAR_WIDTH_DEFAULT,
    };
    snprintf(s.theme_id, sizeof(s.theme_id), "%s", SOL_SETTINGS_THEME_ID_DEFAULT);
    snprintf(s.bg_effect_id, sizeof(s.bg_effect_id), "%s",
             SOL_SETTINGS_BG_EFFECT_ID_DEFAULT);
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
        } else if (strcmp(key, "style") == 0) {
            jp_string(j, s->theme_id, sizeof(s->theme_id));
        } else if (strcmp(key, "effect") == 0) {
            jp_string(j, s->bg_effect_id, sizeof(s->bg_effect_id));
        } else if (strcmp(key, "opacity") == 0) {
            float v = jp_float(j);
            if (!isnan(v) &&
                v >= SOL_SETTINGS_BG_OPACITY_MIN &&
                v <= SOL_SETTINGS_BG_OPACITY_MAX)
                s->bg_opacity = v;
        } else if (strcmp(key, "bg_blur") == 0) {
            float v = jp_float(j);
            if (!isnan(v) &&
                v >= SOL_SETTINGS_BG_BLUR_MIN &&
                v <= SOL_SETTINGS_BG_BLUR_MAX)
                s->bg_blur = v;
        } else if (strcmp(key, "buffer_blur") == 0) {
            float v = jp_float(j);
            if (!isnan(v) &&
                v >= SOL_SETTINGS_BUFFER_BLUR_MIN &&
                v <= SOL_SETTINGS_BUFFER_BLUR_MAX)
                s->buffer_blur = v;
        } else {
            jp_skip_value(j);
        }
        jp_skip_ws(j);
        if (*j->p == ',') ++j->p;
    }
}

/*
 * Parse appearance overlay object from JSON.
 *
 * j  Parser cursor positioned at appearance object.
 * s  Settings struct to populate.
 */
static void jp_parse_appearance(JP *j, SolSettings *s)
{
    if (!jp_expect(j, '{')) return;
    while (*j->p) {
        jp_skip_ws(j);
        if (*j->p == '}') { ++j->p; break; }
        char key[64];
        if (!jp_string(j, key, sizeof(key))) break;
        if (!jp_expect(j, ':')) break;

#define PARSE_FLOAT_FIELD(field_name, field, mn, mx)  \
        if (strcmp(key, field_name) == 0) {           \
            float v = jp_float(j);                    \
            if (!isnan(v) && v >= (mn) && v <= (mx))  \
                s->field = v;                         \
        } else

        PARSE_FLOAT_FIELD("corner_radius",    corner_radius,    SOL_SETTINGS_CORNER_RADIUS_MIN,    SOL_SETTINGS_CORNER_RADIUS_MAX)
        PARSE_FLOAT_FIELD("panel_blur",       panel_blur,       SOL_SETTINGS_PANEL_BLUR_MIN,       SOL_SETTINGS_PANEL_BLUR_MAX)
        PARSE_FLOAT_FIELD("titlebar_blur",    titlebar_blur,    SOL_SETTINGS_TITLEBAR_BLUR_MIN,    SOL_SETTINGS_TITLEBAR_BLUR_MAX)
        PARSE_FLOAT_FIELD("panel_opacity",    panel_opacity,    SOL_SETTINGS_PANEL_OPACITY_MIN,    SOL_SETTINGS_PANEL_OPACITY_MAX)
        PARSE_FLOAT_FIELD("scrollbar_width",  scrollbar_width,  SOL_SETTINGS_SCROLLBAR_WIDTH_MIN,  SOL_SETTINGS_SCROLLBAR_WIDTH_MAX)
        /* else */
        {
            jp_skip_value(j);
        }
#undef PARSE_FLOAT_FIELD

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
        } else if (strcmp(key, "appearance") == 0) {
            jp_parse_appearance(j, s);
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
    char esc_theme[sizeof(settings->theme_id) * 2 + 1];
    {
        const char *src = settings->bg_effect_id;
        char       *dst = esc_id;
        while (*src) {
            if (*src == '"' || *src == '\\') *dst++ = '\\';
            *dst++ = *src++;
        }
        *dst = '\0';
    }
    {
        const char *src = settings->theme_id;
        char *dst = esc_theme;
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
        "    \"style\": \"%s\",\n"
        "    \"effect\": \"%s\",\n"
        "    \"opacity\": %.2f,\n"
        "    \"bg_blur\": %.2f,\n"
        "    \"buffer_blur\": %.2f\n"
        "  },\n"
        "  \"appearance\": {\n"
        "    \"corner_radius\": %.2f,\n"
        "    \"panel_blur\": %.2f,\n"
        "    \"titlebar_blur\": %.2f,\n"
        "    \"panel_opacity\": %.2f,\n"
        "    \"scrollbar_width\": %.2f\n"
        "  }\n"
        "}\n",
        (double)settings->ui_scale,
        esc_theme,
        esc_id,
        (double)settings->bg_opacity,
        (double)settings->bg_blur,
        (double)settings->buffer_blur,
        (double)settings->corner_radius,
        (double)settings->panel_blur,
        (double)settings->titlebar_blur,
        (double)settings->panel_opacity,
        (double)settings->scrollbar_width);

    fclose(fp);
    return n > 0;
}

/*
 * Build a CSS override snippet encoding the appearance overlay settings.
 *
 * The generated CSS overrides border-radius, scrollbar width/radius,
 * and panel opacity. It is appended after the active theme so it wins by
 * declaration order.
 *
 * Corner radius is applied to every interactive element that has a
 * `border-radius: 0px` declaration in the default stylesheet so the slider
 * produces a uniform effect across the whole UI.
 *
 * Panel opacity uses the CSS `opacity` property (theme-independent) rather
 * than baking a specific rgba colour, so it works with any registered theme.
 */
int sol_settings_build_appearance_css(const SolSettings *settings,
                                      char *buf, int bufsz)
{
    if (!settings || !buf || bufsz <= 0) return 0;

    float cr      = settings->corner_radius;
    float sw      = settings->scrollbar_width;
    float sr      = sw * 0.5f;
    float pblur   = settings->panel_blur;
    float tblur   = settings->titlebar_blur;
    float op      = settings->panel_opacity;
    float control_radius = fminf(cr * 0.5f, 10.0f);

    int written = snprintf(buf, (size_t)bufsz,
        "/* sol appearance overlay */"
        /* Scrollbar geometry — wildcard wins by appending after theme. */
        "* { scrollbar-width: %.1fpx; scrollbar-radius: %.1fpx;"
        " scrollbar-track-color: transparent; }"
        ".buffer-scrollbar { width: %.1fpx; padding: 0px 2px; background: transparent; }"
        ".buffer-scrollbar-spacer { width: 100%%; }"
        ".buffer-hscrollbar { height: %.1fpx; padding: 2px 0px; background: transparent; }"
        ".buffer-hscrollbar-spacer { height: 100%%; }"
        ".buffer-scrollbar-thumb, .buffer-scrollbar-thumb-active {"
        " width: 100%%; border-radius: %.1fpx; }"
        ".buffer-hscrollbar-thumb, .buffer-hscrollbar-thumb-active {"
        " height: 100%%; border-radius: %.1fpx; }"
        ".buffer-tabs-row { border-top-left-radius: %.1fpx;"
        " border-top-right-radius: %.1fpx; overflow: hidden; }"
        ".buffer-body, .buffer-scroll-row { border-bottom-left-radius: %.1fpx;"
        " border-bottom-right-radius: %.1fpx; overflow: hidden; }"
        ".buffer-gutter-col { border-bottom-left-radius: %.1fpx; }"
        /* Buffer panes + side panel: corner-radius, backdrop blur, opacity. */
        ".tree-panel, .plugin-side-panel, .buffer-pane, .term-panel,"
        ".welcome-pane, .cf-panel, .status-bar {"
        "  border-radius: %.1fpx;"
        "  backdrop-filter: blur(%.1fpx);"
        "  opacity: %.3f;"
        "}"
        /* Auxiliary dialog windows (Settings, Plugin Manager, File Picker,
           Search) are plain OS-decorated square windows, not borderless
           floating panels — share blur/opacity but never corner-radius,
           which would round their root div against nothing and expose
           the window's default backdrop at the corners. */
        ".fp-root, .search-root-window, .pm-root, .sw-root {"
        "  backdrop-filter: blur(%.1fpx);"
        "  opacity: %.3f;"
        "}"
        /* Titlebar: independent backdrop blur (opacity shared) */
        ".ca-titlebar {"
        "  backdrop-filter: blur(%.1fpx);"
        "  opacity: %.3f;"
        "}"
        /* Corner-radius on every interactive / card element. */
        ".ca-popup-card,"
        ".ca-popup-btn,"
        ".ca-select-popup,"
        ".ca-tooltip,"
        ".ca-context-menu,"
        ".ca-menubar-popup,"
        ".ca-menubar-popup .ca-titlebar-menu-item,"
        ".ca-titlebar-menu-item,"
        ".ca-titlebar-control,"
        ".ca-overlay-hover,"
        ".buffer-tab,"
        ".buffer-tab-close,"
        ".buffer-hscrollbar-thumb,"
        ".buffer-hscrollbar-thumb-active,"
        ".fp-row,"
        ".fp-new-folder-input,"
        ".fp-action-new-folder,"
        ".fp-nf-create,"
        ".fp-nf-cancel,"
        ".fp-up-btn,"
        ".fp-crumb-btn,"
        ".fp-colhdr-btn,"
        ".fp-colhdr-btn-end,"
        ".fp-action-cancel,"
        ".fp-action-primary,"
        ".pm-btn,"
        ".pm-btn-enable,"
        ".pm-btn-disable,"
        ".pm-search-input,"
        ".scm-header-action,"
        ".scm-action,"
        ".scm-row-action,"
        ".scm-row-danger,"
        ".scm-section-action,"
        ".scm-primary-action,"
        ".scm-danger-action,"
        ".scm-commit-input,"
        ".scm-branch-input,"
        ".scm-branch-row,"
        ".scm-branch-row-current,"
        ".search-result,"
        ".search-input,"
        ".sw-scale-input,"
        ".sw-select,"
        ".sw-tab-btn,"
        ".pm-item,"
        ".tree-row,"
        ".tree-sticky-row,"
        ".cf-row,"
        ".cf-row-key,"
        ".status-bar-badge,"
        ".pm-badge,"
        ".term-tab,"
        ".term-tab-active,"
        ".term-tab-close,"
        ".welcome-btn,"
        ".welcome-btn-primary,"
        ".scm-tab,"
        ".scm-tab-active"
        " { border-radius: %.1fpx; }",
        (double)sw, (double)sr,
        (double)sw, (double)sw, (double)sr, (double)sr,
        (double)cr, (double)cr, (double)cr, (double)cr, (double)cr,
        (double)cr, (double)pblur, (double)op,
        (double)pblur, (double)op,
        (double)tblur, (double)op,
        (double)control_radius);

    if (written < 0 || written >= bufsz) return 0;
    return written;
}
