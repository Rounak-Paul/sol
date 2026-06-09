// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_config.c — Loader for $HOME/.sol/bindings.conf.
 *
 * Hand-rolled parser (no external deps). Two directive shapes:
 *
 *     leader <modifier>
 *         Set the leader key.  <modifier> is one of: ctrl  alt  super  shift.
 *         Must appear before any `bind` lines.  Default is ctrl.
 *
 *     bind L <key1> ... <keyN> <action>
 *         Bind a chord to an action.  L is the leader placeholder; you may
 *         also write the literal modifier name (e.g. `ctrl`) as long as it
 *         matches the declared leader.  Each key may carry `shift+`, `alt+`,
 *         or `super+` prefix (case-insensitive, any combination).  The last
 *         token is the action name.
 *
 * All other line shapes (comments, blank, unknown keyword) are silently
 * ignored.  Per-line parse errors print a single `bindings.conf: line N:`
 * warning to stderr and skip the line — the loader keeps going so a typo
 * doesn't kill the whole keymap.
 */

#include "sol_config.h"

#include "sol_platform.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sol_input.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

static const char *const SOL_DEFAULT_BINDINGS_CONF =
    "# Sol key bindings.\n"
    "#\n"
    "# leader <modifier>          Declare the leader key (ctrl / alt / super / shift).\n"
    "# bind L <key>... <action>   Bind a chord to an action.\n"
    "#\n"
    "# 'L' is the leader placeholder — it expands to whichever modifier was\n"
    "# declared with 'leader'. You may also write the modifier name directly\n"
    "# (e.g. 'ctrl') as long as it matches the declared leader. Put 'leader'\n"
    "# before any 'bind' lines.\n"
    "#\n"
    "# Each step key after L may carry 'shift+', 'alt+', or 'super+' prefix\n"
    "# (case-insensitive). The last token on a bind line is the action.\n"
    "#\n"
    "# Available actions (each publishes sol.command.invoked):\n"
    "#\n"
    "# Buffer\n"
    "#   buffer.new              Open a new empty buffer.\n"
    "#   buffer.open             Open a file from disk into a buffer.\n"
    "#   buffer.close            Close the active buffer.\n"
    "#   buffer.focus.previous   Switch to the previously focused buffer.\n"
    "#   buffer.focus.first      Focus the first buffer in tab order.\n"
    "#   buffer.focus.last       Focus the last buffer in tab order.\n"
    "#   buffer.cycle.next       Cycle the active leaf to the next buffer.\n"
    "#   buffer.cycle.prev       Cycle the active leaf to the previous buffer.\n"
    "#\n"
    "# Pane\n"
    "#   pane.split.vertical     Split the active pane vertically.\n"
    "#   pane.split.horizontal   Split the active pane horizontally.\n"
    "#   pane.focus.next         Move focus to the next pane.\n"
    "#   pane.focus.prev         Move focus to the previous pane.\n"
    "#\n"
    "# Explorer\n"
    "#   explorer.focus.toggle   Toggle explorer panel focus/visibility.\n"
    "#   explorer.open           Open a folder in the explorer panel.\n"
    "#\n"
    "# Find\n"
    "#   find.files              Fuzzy-search files in the workspace.\n"
    "#   find.grep               Search text across workspace files.\n"
    "#\n"
    "# Edit  (scope prefixes: w = word, l = line; none = char / selection)\n"
    "#   edit.copy               Copy selection to clipboard.\n"
    "#   edit.copy_word          Copy word at cursor to clipboard.\n"
    "#   edit.copy_line          Copy current line to clipboard.\n"
    "#   edit.cut                Cut selection to clipboard.\n"
    "#   edit.paste              Paste clipboard at cursor.\n"
    "#   edit.paste_line         Paste clipboard as a new line below cursor.\n"
    "#   edit.undo               Undo last edit.\n"
    "#   edit.redo               Redo last undone edit.\n"
    "#   edit.select_all         Select entire buffer.\n"
    "#   edit.delete_char        Delete char forward (or selection if active).\n"
    "#   edit.delete_word        Delete word forward.\n"
    "#   edit.delete_word_back   Delete word backward.\n"
    "#   edit.delete_line        Delete current line.\n"
    "#\n"
    "# Plugins may register additional actions. Add bindings here to wire\n"
    "# them to chords without touching Sol's source.\n"
    "\n"
    "leader ctrl\n"
    "\n"
    "bind L b c            buffer.new\n"
    "bind L b o            buffer.open\n"
    "bind L b x            buffer.close\n"
    "bind L b b            buffer.focus.previous\n"
    "bind L b n            buffer.cycle.next\n"
    "bind L b p            buffer.cycle.prev\n"
    "bind L b shift+n      buffer.focus.last\n"
    "bind L b shift+p      buffer.focus.first\n"
    "bind L p v            pane.split.vertical\n"
    "bind L p h            pane.split.horizontal\n"
    "bind L p n            pane.focus.next\n"
    "bind L p p            pane.focus.prev\n"
    "bind L e e            explorer.focus.toggle\n"
    "bind L e o            explorer.open\n"
    "bind L f f            find.files\n"
    "bind L f g            find.grep\n"
    "bind L e c            edit.copy\n"
    "bind L e w c          edit.copy_word\n"
    "bind L e l c          edit.copy_line\n"
    "bind L e x            edit.cut\n"
    "bind L e p            edit.paste\n"
    "bind L e l p          edit.paste_line\n"
    "bind L e u            edit.undo\n"
    "bind L e r            edit.redo\n"
    "bind L e a            edit.select_all\n"
    "bind L e d            edit.delete_char\n"
    "bind L e w d          edit.delete_word\n"
    "bind L e w backspace  edit.delete_word_back\n"
    "bind L e l d          edit.delete_line\n";

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static char *sol_path_join(const char *a, const char *b)
{
    return sol_platform_path_join(a, b);
}

static bool sol_mkdir_p(const char *path)
{
    return sol_platform_mkdir_p(path);
}

/*
 * Return (and create if absent) the Sol configuration directory path.
 *
 * Returns Heap-allocated absolute path to $HOME/.sol (or platform equivalent),
 *         or NULL if the path cannot be determined or created.
 */
char *sol_config_dir(void)
{
    char *dir = sol_platform_config_home_dir();
    if (!dir) return NULL;
    if (!sol_mkdir_p(dir)) {
        fprintf(stderr, "sol: cannot create config dir '%s': %s\n",
                dir, strerror(errno));
        free(dir);
        return NULL;
    }
    return dir;
}

/*
 * Build a full path to a file inside the Sol configuration directory.
 *
 * filename  File name relative to the config dir (e.g. "bindings.conf").
 * Returns   Heap-allocated absolute path, or NULL on failure.
 */
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

/*
 * Write the built-in default bindings.conf template to disk.
 *
 * path    Absolute path of the file to create.
 * Returns true on success.
 */
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

/*
 * Tokenise a line in place by NUL-terminating each whitespace-separated token.
 *
 * line        Mutable line buffer; whitespace between tokens is overwritten.
 * tokens      Caller-allocated array to receive pointers to each token start.
 * max_tokens  Capacity of the tokens array.
 * Returns     Number of tokens found.
 */
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

/* Return the canonical lowercase name of a modifier mask (leader key). */
static const char *sol_modifier_name(SolModifierMask mod)
{
    switch (mod) {
    case SOL_MOD_CTRL:  return "ctrl";
    case SOL_MOD_ALT:   return "alt";
    case SOL_MOD_SUPER: return "super";
    case SOL_MOD_SHIFT: return "shift";
    default:            return "ctrl";
    }
}

/*
 * Parse and register one `bind` directive from the config file.
 *
 * ui       UI system to register the command flow into.
 * tokens   Null-terminated token array produced by sol_tokenise_inplace().
 * ntokens  Number of tokens in the array.
 * line_no  Source line number, used in diagnostic messages.
 * Returns  true if the binding was successfully registered.
 */
static bool sol_register_bind_line(SolUISystem *ui, char **tokens, size_t ntokens,
                                   size_t line_no)
{
    /* Expect: bind <L|leader-name> <step1> ... <stepK> <action>
       So ntokens >= 4 (bind + leader + at-least-one-step + action). */
    if (ntokens < 4u) {
        fprintf(stderr, "bindings.conf: line %zu: too few tokens for `bind`\n",
                line_no);
        return false;
    }
    /* Accept "L" (portable leader placeholder) or the literal name of the
       configured leader modifier.  The literal name allows old configs written
       before the `leader` directive existed to keep working unchanged. */
    const SolModifierMask leader_mod = sol_ui_system_leader_modifier(ui);
    const bool leader_ok =
        sol_streq_ci(tokens[1], "L") ||
        sol_streq_ci(tokens[1], sol_modifier_name(leader_mod));
    if (!leader_ok) {
        fprintf(stderr,
                "bindings.conf: line %zu: second token must be `L` "
                "(leader placeholder) or `%s` (current leader name)\n",
                line_no, sol_modifier_name(leader_mod));
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

/*
 * Load key bindings from $HOME/.sol/bindings.conf and register them.
 *
 * Writes a default config file on first launch. Lines that fail to parse
 * are skipped with a warning so a single typo doesn't break the whole keymap.
 *
 * ui      UI system to register the parsed command flows into.
 * Returns Number of successfully registered bindings, or -1 on fatal error.
 */
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

        if (sol_streq_ci(tokens[0], "leader")) {
            if (n < 2u) {
                fprintf(stderr,
                        "bindings.conf: line %zu: `leader` requires a modifier name\n",
                        line_no);
            } else {
                SolModifierMask mod = SOL_MOD_NONE;
                if      (sol_streq_ci(tokens[1], "ctrl"))  mod = SOL_MOD_CTRL;
                else if (sol_streq_ci(tokens[1], "alt"))   mod = SOL_MOD_ALT;
                else if (sol_streq_ci(tokens[1], "super")) mod = SOL_MOD_SUPER;
                else if (sol_streq_ci(tokens[1], "shift")) mod = SOL_MOD_SHIFT;
                else {
                    fprintf(stderr,
                            "bindings.conf: line %zu: unknown leader `%s` "
                            "(expected ctrl / alt / super / shift)\n",
                            line_no, tokens[1]);
                    mod = SOL_MOD_NONE;
                }
                if (mod != SOL_MOD_NONE) {
                    sol_ui_system_set_leader_modifier(ui, mod);
                }
            }
            continue;
        }

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
