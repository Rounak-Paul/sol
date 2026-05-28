// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_config.c — Loader for $HOME/.sol/bindings.conf.
 *
 * Hand-rolled parser (no external deps). Grammar is a single line shape:
 *
 *     bind <key1> <key2> ... <keyN> <action>
 *
 * where <key1> MUST be `ctrl` (leader) and is stripped; each remaining
 * key may be prefixed by `shift+`, `alt+`, `super+` (case-insensitive,
 * any combination). The last token on a `bind` line is the action.
 *
 * All other line shapes (comments, blank, unknown keyword) are
 * silently ignored. Per-line parse errors print a single
 * `bindings.conf: line N:` warning to stderr and skip the line — the
 * loader keeps going so a typo doesn't kill the whole keymap.
 */

#include "sol_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define SOL_PATH_SEP '\\'
#define SOL_MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#define SOL_PATH_SEP '/'
#define SOL_MKDIR(path) mkdir((path), 0755)
#endif

#include "sol_input.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

static const char *const SOL_DEFAULT_BINDINGS_CONF =
    "# Sol key bindings.\n"
    "#\n"
    "# Format: bind <chord> <action>\n"
    "#\n"
    "# A chord is a whitespace-separated sequence of keys. Each key may\n"
    "# be prefixed by 'shift+', 'alt+', or 'super+' (case-insensitive).\n"
    "# The first key MUST be 'ctrl' (the leader); it is implicit on the\n"
    "# remaining steps.\n"
    "#\n"
    "# Available actions (each publishes sol.command.invoked):\n"
    "#   buffer.focus.last       Show the previously focused buffer.\n"
    "#   buffer.cycle.next       Cycle the active pane to the next buffer.\n"
    "#   buffer.cycle.prev       Cycle the active pane to the previous buffer.\n"
    "#   pane.cycle.next         Move focus to the next pane.\n"
    "#   pane.cycle.prev         Move focus to the previous pane.\n"
    "#   pane.split.horizontal   Split the active pane horizontally.\n"
    "#   pane.split.vertical     Split the active pane vertically.\n"
    "#\n"
    "# Plugins may register additional actions. Add bindings here to wire\n"
    "# them to chords without touching Sol's source.\n"
    "\n"
    "bind ctrl b b            buffer.focus.last\n"
    "bind ctrl b n            buffer.cycle.next\n"
    "bind ctrl b shift+n      buffer.cycle.prev\n"
    "bind ctrl w n            pane.cycle.next\n"
    "bind ctrl w shift+n      pane.cycle.prev\n"
    "bind ctrl w h            pane.split.horizontal\n"
    "bind ctrl w v            pane.split.vertical\n";

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static char *sol_path_join(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    const size_t la = strlen(a);
    const size_t lb = strlen(b);
    const bool need_sep = (la > 0u && a[la - 1u] != SOL_PATH_SEP);
    char *out = (char *)malloc(la + (need_sep ? 1u : 0u) + lb + 1u);
    if (!out) return NULL;
    memcpy(out, a, la);
    size_t off = la;
    if (need_sep) {
        out[off++] = SOL_PATH_SEP;
    }
    memcpy(out + off, b, lb);
    out[off + lb] = '\0';
    return out;
}

static bool sol_mkdir_p(const char *path)
{
    /* Try once outright; only fall back to walking on ENOENT to keep
       the common path cheap. */
    if (SOL_MKDIR(path) == 0 || errno == EEXIST) {
        return true;
    }
    if (errno != ENOENT) {
        return false;
    }

    /* Walk the path creating parents as needed. */
    char *copy = strdup(path);
    if (!copy) return false;
    for (char *p = copy + 1; *p; ++p) {
        if (*p == SOL_PATH_SEP) {
            *p = '\0';
            if (SOL_MKDIR(copy) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            *p = SOL_PATH_SEP;
        }
    }
    const bool ok = (SOL_MKDIR(copy) == 0 || errno == EEXIST);
    free(copy);
    return ok;
}

char *sol_config_dir(void)
{
#if defined(_WIN32)
    const char *home = getenv("APPDATA");
    const char *name = "sol";
#else
    const char *home = getenv("HOME");
    const char *name = ".sol";
#endif
    if (!home || home[0] == '\0') return NULL;
    char *dir = sol_path_join(home, name);
    if (!dir) return NULL;
    if (!sol_mkdir_p(dir)) {
        fprintf(stderr, "sol: cannot create config dir '%s': %s\n",
                dir, strerror(errno));
        free(dir);
        return NULL;
    }
    return dir;
}

char *sol_config_path(const char *filename)
{
    if (!filename) return NULL;
    char *dir = sol_config_dir();
    if (!dir) return NULL;
    char *path = sol_path_join(dir, filename);
    free(dir);
    return path;
}

/* ------------------------------------------------------------------ */
/* Default-file emission                                               */
/* ------------------------------------------------------------------ */

static bool sol_write_default_bindings(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "sol: cannot create '%s': %s\n", path, strerror(errno));
        return false;
    }
    const size_t len = strlen(SOL_DEFAULT_BINDINGS_CONF);
    const bool ok = (fwrite(SOL_DEFAULT_BINDINGS_CONF, 1u, len, fp) == len);
    fclose(fp);
    if (!ok) {
        fprintf(stderr, "sol: short write to '%s'\n", path);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Token / key parsing                                                 */
/* ------------------------------------------------------------------ */

/* Case-insensitive ASCII compare. */
static bool sol_streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Map a named key (lowercased) to a SolKeyCode. Returns false when the
   name is not a known special key. Single-character names are NOT
   handled here — the caller deals with ASCII keys directly. */
static bool sol_keyname_to_code(const char *name, SolKeyCode *out)
{
    static const struct { const char *name; SolKeyCode code; } table[] = {
        { "escape",    SOL_KEY_ESCAPE    },
        { "esc",       SOL_KEY_ESCAPE    },
        { "enter",     SOL_KEY_ENTER     },
        { "return",    SOL_KEY_ENTER     },
        { "tab",       SOL_KEY_TAB       },
        { "backspace", SOL_KEY_BACKSPACE },
        { "delete",    SOL_KEY_DELETE    },
        { "insert",    SOL_KEY_INSERT    },
        { "left",      SOL_KEY_LEFT      },
        { "right",     SOL_KEY_RIGHT     },
        { "up",        SOL_KEY_UP        },
        { "down",      SOL_KEY_DOWN      },
        { "home",      SOL_KEY_HOME      },
        { "end",       SOL_KEY_END       },
        { "pageup",    SOL_KEY_PAGE_UP   },
        { "pagedown",  SOL_KEY_PAGE_DOWN },
        { "space",     ' '               },
    };
    for (size_t i = 0u; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (sol_streq_ci(name, table[i].name)) {
            *out = table[i].code;
            return true;
        }
    }
    return false;
}

/* Parse one chord token like "shift+n" or "ctrl" or "h". Writes the
   key code and modifier mask. Returns false on a malformed token. */
static bool sol_parse_chord_token(const char *token,
                                  SolKeyCode      *out_key,
                                  SolModifierMask *out_mods)
{
    if (!token || token[0] == '\0') return false;

    SolModifierMask mods = SOL_MOD_NONE;

    /* Walk `mod+mod+...+key` left to right. Each `+`-delimited piece is
       either a known modifier or the terminal key name. */
    char buf[64];
    size_t cursor = 0u;
    const char *p = token;
    for (;;) {
        /* Copy next piece into buf. */
        size_t bi = 0u;
        while (*p && *p != '+' && bi + 1u < sizeof(buf)) {
            buf[bi++] = *p++;
        }
        buf[bi] = '\0';
        if (bi == 0u) return false;

        if (*p == '+') {
            /* Modifier piece. */
            if (sol_streq_ci(buf, "shift"))      mods |= SOL_MOD_SHIFT;
            else if (sol_streq_ci(buf, "alt"))   mods |= SOL_MOD_ALT;
            else if (sol_streq_ci(buf, "super")) mods |= SOL_MOD_SUPER;
            else if (sol_streq_ci(buf, "ctrl"))  mods |= SOL_MOD_CTRL;
            else return false;
            ++p;   /* skip '+' */
            ++cursor;
            continue;
        }

        /* Terminal key piece. */
        SolKeyCode code = SOL_KEY_UNKNOWN;
        if (sol_keyname_to_code(buf, &code)) {
            *out_key  = code;
            *out_mods = mods;
            return true;
        }
        if (bi == 1u) {
            /* Single ASCII character. Uppercase letters fold to upper
               so 'n' and 'N' both yield key 'N' (the modifier mask
               carries the shift distinction). */
            unsigned char c = (unsigned char)buf[0];
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - ('a' - 'A'));
            *out_key  = (SolKeyCode)c;
            *out_mods = mods;
            return true;
        }
        return false;
    }
}

/* In-place tokeniser: rewrites consecutive whitespace runs to NULs,
   stores token start pointers. Returns token count. */
static size_t sol_tokenise_inplace(char *line, char **tokens, size_t max_tokens)
{
    size_t n = 0u;
    char *p = line;
    while (*p && n < max_tokens) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && !isspace((unsigned char)*p)) ++p;
        if (*p) {
            *p = '\0';
            ++p;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Line handler                                                        */
/* ------------------------------------------------------------------ */

static bool sol_register_bind_line(SolUISystem *ui, char **tokens, size_t ntokens,
                                   size_t line_no)
{
    /* Expect: bind <ctrl> <step1> ... <stepK> <action>
       So ntokens >= 4 (bind + ctrl + at-least-one-step + action). */
    if (ntokens < 4u) {
        fprintf(stderr, "bindings.conf: line %zu: too few tokens for `bind`\n",
                line_no);
        return false;
    }
    if (!sol_streq_ci(tokens[1], "ctrl")) {
        fprintf(stderr, "bindings.conf: line %zu: leader must be `ctrl`\n",
                line_no);
        return false;
    }

    const char *action = tokens[ntokens - 1u];
    const size_t step_count = ntokens - 3u;   /* exclude bind, ctrl, action */
    if (step_count == 0u || step_count > SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        fprintf(stderr, "bindings.conf: line %zu: %zu chord steps (max %u)\n",
                line_no, step_count,
                (unsigned)SOL_UI_MAX_FLOW_SEQUENCE_LEN);
        return false;
    }

    SolKeyCode      sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    SolModifierMask step_mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    for (size_t i = 0u; i < step_count; ++i) {
        if (!sol_parse_chord_token(tokens[2u + i], &sequence[i], &step_mods[i])) {
            fprintf(stderr, "bindings.conf: line %zu: bad chord token `%s`\n",
                    line_no, tokens[2u + i]);
            return false;
        }
        /* The leader modifier must never appear on a per-step token. */
        if ((step_mods[i] & SOL_MOD_CTRL) != 0u) {
            fprintf(stderr, "bindings.conf: line %zu: `ctrl+` is implicit; "
                    "drop it from step `%s`\n", line_no, tokens[2u + i]);
            step_mods[i] = (SolModifierMask)(step_mods[i] & ~SOL_MOD_CTRL);
        }
    }

    SolCommandFlowDesc desc = {0};
    desc.action          = action;
    desc.label           = action;   /* fallback; can be richer later */
    desc.sequence        = sequence;
    desc.step_modifiers  = step_mods;
    desc.sequence_length = step_count;
    desc.key             = sequence[0];
    desc.callback        = NULL;     /* event-driven dispatch */
    desc.user_data       = NULL;
    if (!sol_ui_system_register_command_flow(ui, &desc)) {
        fprintf(stderr, "bindings.conf: line %zu: registration failed for `%s`\n",
                line_no, action);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public loader                                                       */
/* ------------------------------------------------------------------ */

int sol_config_load_bindings(SolUISystem *ui)
{
    if (!ui) return -1;

    char *path = sol_config_path("bindings.conf");
    if (!path) return -1;

    /* Auto-emit defaults the first time. The file is then user-editable. */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "sol: stat('%s'): %s\n", path, strerror(errno));
            free(path);
            return -1;
        }
        if (!sol_write_default_bindings(path)) {
            free(path);
            return -1;
        }
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "sol: cannot open '%s': %s\n", path, strerror(errno));
        free(path);
        return -1;
    }

    int    registered = 0;
    char   line[1024];
    size_t line_no = 0u;
    while (fgets(line, sizeof(line), fp)) {
        ++line_no;

        /* Strip inline `#` comments. */
        for (char *p = line; *p; ++p) {
            if (*p == '#') { *p = '\0'; break; }
        }

        char *tokens[8 + SOL_UI_MAX_FLOW_SEQUENCE_LEN];
        const size_t n = sol_tokenise_inplace(
            line, tokens, sizeof(tokens) / sizeof(tokens[0]));
        if (n == 0u) continue;

        if (sol_streq_ci(tokens[0], "bind")) {
            if (sol_register_bind_line(ui, tokens, n, line_no)) {
                ++registered;
            }
            continue;
        }

        fprintf(stderr, "bindings.conf: line %zu: unknown directive `%s`\n",
                line_no, tokens[0]);
    }

    fclose(fp);
    free(path);
    return registered;
}
