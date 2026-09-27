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
 *     unbind <action>
 *         Leave <action> without a chord, overriding its built-in or
 *         plugin default. The command itself stays available to menus.
 *
 * bind/unbind lines form the user keymap layer
 * (sol_ui_system_set_keymap_override): they win over every default chord,
 * including defaults that plugins register after this file is loaded.
 *
 * The Settings window edits this file surgically (sol_config_save_binding,
 * sol_config_save_leader): only the affected line is rewritten, so hand
 * edits and comments elsewhere in the file survive.
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
    "# unbind <action>            Remove the chord bound to an action.\n"
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
    "#   buffer.save             Save the active buffer to its file.\n"
    "#   buffer.save_all         Save every buffer with unsaved changes.\n"
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
    "# Terminal\n"
    "#   terminal.toggle            Show/focus terminal; defocus if already focused.\n"
    "#   terminal.tab.new           Open a new terminal tab and focus it.\n"
    "#   terminal.tab.next          Switch to next terminal tab and focus it.\n"
    "#   terminal.tab.prev          Switch to prev terminal tab and focus it.\n"
    "#   terminal.kill              Kill the active terminal tab.\n"
    "#   terminal.position.bottom   Dock terminal at the bottom.\n"
    "#   terminal.position.right    Dock terminal on the right.\n"
    "#   terminal.position.float    Float terminal as a centered overlay.\n"
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
    "bind L b s            buffer.save\n"
    "bind L b shift+s      buffer.save_all\n"
    "bind L p v            pane.split.vertical\n"
    "bind L p h            pane.split.horizontal\n"
    "bind L p n            pane.focus.next\n"
    "bind L p p            pane.focus.prev\n"
    "bind L x x            explorer.focus.toggle\n"
    "bind L x o            explorer.open\n"
    "bind L f f            find.files\n"
    "bind L f g            find.grep\n"
    "bind L t t            terminal.toggle\n"
    "bind L s c            project.create\n"
    "bind L s n            project.next\n"
    "bind L s p            project.previous\n"
    "bind L s x            project.close\n"
    "bind L s s            project.switcher\n"
    "bind L t c            terminal.tab.new\n"
    "bind L t n            terminal.tab.next\n"
    "bind L t p            terminal.tab.prev\n"
    "bind L t x            terminal.kill\n"
    "bind L t h            terminal.position.bottom\n"
    "bind L t v            terminal.position.right\n"
    "bind L t f            terminal.position.float\n"
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
 * Atomically replace a file's contents: write a per-process temp file,
 * fsync it, then rename it over the target, so readers (including other
 * Sol instances watching the config dir) never observe a partial file.
 *
 * path  Absolute path of the file to replace.
 * data  Bytes to write.
 * len   Number of bytes in data.
 * Returns true on success.
 */
static bool sol_config_write_atomic(const char *path, const char *data, size_t len)
{
    char tmp_path[4160];
    const int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp%ld",
                           path, sol_platform_process_id());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        fprintf(stderr, "sol: path too long for '%s'\n", path);
        return false;
    }

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        fprintf(stderr, "sol: cannot create '%s': %s\n", tmp_path, strerror(errno));
        return false;
    }
    bool ok = (fwrite(data, 1u, len, fp) == len);
    ok = ok && (fflush(fp) == 0) && sol_platform_sync_file(fp);
    if (fclose(fp) != 0) ok = false;

    if (ok) ok = sol_platform_replace_file(tmp_path, path);
    if (!ok) {
        remove(tmp_path);
        fprintf(stderr, "sol: failed to write '%s'\n", path);
    }
    return ok;
}

/*
 * Write the built-in default bindings.conf template to disk.
 *
 * path    Absolute path of the file to create.
 * Returns true on success.
 */
static bool sol_write_default_bindings(const char *path)
{
    /* An interrupted direct write leaves a truncated or empty
       bindings.conf, which is a config that parses but binds nothing. */
    return sol_config_write_atomic(path, SOL_DEFAULT_BINDINGS_CONF,
                                   strlen(SOL_DEFAULT_BINDINGS_CONF));
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
/* Chord text <-> key sequence                                         */
/* ------------------------------------------------------------------ */

/*
 * Parse a leader-relative chord such as "b shift+s" into key steps.
 *
 * text        Whitespace-separated step tokens (the leader is implicit).
 * leader      Active leader modifier. A step carrying it, or ctrl (which
 *             the loader always treats as implicit), is rejected.
 * sequence    Receives SOL_UI_MAX_FLOW_SEQUENCE_LEN key codes at most.
 * modifiers   Receives the per-step non-leader modifier masks.
 * out_length  Receives the number of steps.
 * Returns     false for an empty, malformed, or over-long chord, or one
 *             using '#', which bindings.conf reserves for comments.
 */
bool sol_config_parse_chord(const char      *text,
                            SolModifierMask  leader,
                            SolKeyCode       sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN],
                            SolModifierMask  modifiers[SOL_UI_MAX_FLOW_SEQUENCE_LEN],
                            size_t          *out_length)
{
    if (!text || !sequence || !modifiers || !out_length) return false;

    char line[256];
    const size_t text_len = strlen(text);
    if (text_len >= sizeof(line) || strchr(text, '#')) return false;
    memcpy(line, text, text_len + 1u);

    char *tokens[SOL_UI_MAX_FLOW_SEQUENCE_LEN + 1u];
    const size_t n = sol_tokenise_inplace(line, tokens,
                                          SOL_UI_MAX_FLOW_SEQUENCE_LEN + 1u);
    if (n == 0u || n > SOL_UI_MAX_FLOW_SEQUENCE_LEN) return false;

    for (size_t i = 0u; i < n; ++i) {
        SolKeyCode      key  = SOL_KEY_UNKNOWN;
        SolModifierMask mods = SOL_MOD_NONE;
        if (!sol_parse_chord_token(tokens[i], &key, &mods)) return false;
        if (key == SOL_KEY_UNKNOWN || (mods & (leader | SOL_MOD_CTRL)) != 0u) return false;
        sequence[i]  = key;
        modifiers[i] = mods;
    }
    *out_length = n;
    return true;
}

/* Write the canonical bindings.conf name of key into out; false if unnamed. */
static bool sol_keycode_to_name(SolKeyCode key, char *out, size_t size)
{
    static const struct { SolKeyCode code; const char *name; } table[] = {
        { SOL_KEY_ESCAPE,    "escape"    },
        { SOL_KEY_ENTER,     "enter"     },
        { SOL_KEY_TAB,       "tab"       },
        { SOL_KEY_BACKSPACE, "backspace" },
        { SOL_KEY_DELETE,    "delete"    },
        { SOL_KEY_INSERT,    "insert"    },
        { SOL_KEY_LEFT,      "left"      },
        { SOL_KEY_RIGHT,     "right"     },
        { SOL_KEY_UP,        "up"        },
        { SOL_KEY_DOWN,      "down"      },
        { SOL_KEY_HOME,      "home"      },
        { SOL_KEY_END,       "end"       },
        { SOL_KEY_PAGE_UP,   "pageup"    },
        { SOL_KEY_PAGE_DOWN, "pagedown"  },
        { ' ',               "space"     },
    };
    for (size_t i = 0u; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].code == key) {
            return snprintf(out, size, "%s", table[i].name) < (int)size;
        }
    }
    if (key > 0x20 && key < 0x7F && key != '#' && key != '+') {
        const int c = (key >= 'A' && key <= 'Z') ? (int)key + ('a' - 'A') : (int)key;
        return snprintf(out, size, "%c", c) < (int)size;
    }
    return false;
}

/*
 * Format key steps as the leader-relative chord text accepted by
 * sol_config_parse_chord, e.g. "b shift+s".
 *
 * sequence   Key codes, one per step.
 * modifiers  Per-step modifier masks (may be NULL for none).
 * length     Number of steps.
 * buf        Destination buffer.
 * size       Size of buf in bytes.
 * Returns    false when a key has no textual name or buf is too small.
 */
bool sol_config_format_chord(const SolKeyCode      *sequence,
                             const SolModifierMask *modifiers,
                             size_t                 length,
                             char                  *buf,
                             size_t                 size)
{
    if (!sequence || !buf || size == 0u || length == 0u) return false;
    buf[0] = '\0';
    size_t used = 0u;
    for (size_t i = 0u; i < length; ++i) {
        char key_name[16];
        if (!sol_keycode_to_name(sequence[i], key_name, sizeof(key_name))) return false;
        const SolModifierMask mods = modifiers ? modifiers[i] : SOL_MOD_NONE;
        const int n = snprintf(buf + used, size - used, "%s%s%s%s%s",
                               i > 0u ? " " : "",
                               (mods & SOL_MOD_SHIFT) ? "shift+" : "",
                               (mods & SOL_MOD_ALT)   ? "alt+"   : "",
                               (mods & SOL_MOD_SUPER) ? "super+" : "",
                               key_name);
        if (n < 0 || (size_t)n >= size - used) return false;
        used += (size_t)n;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Line handler                                                        */
/* ------------------------------------------------------------------ */

/*
 * Return the canonical lowercase name of a leader modifier mask.
 *
 * mod      A single modifier (SOL_MOD_CTRL, _ALT, _SUPER or _SHIFT).
 * Returns  "ctrl", "alt", "super" or "shift"; "ctrl" for anything else.
 */
const char *sol_config_modifier_name(SolModifierMask mod)
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
        sol_streq_ci(tokens[1], sol_config_modifier_name(leader_mod));
    if (!leader_ok) {
        fprintf(stderr,
                "bindings.conf: line %zu: second token must be `L` "
                "(leader placeholder) or `%s` (current leader name)\n",
                line_no, sol_config_modifier_name(leader_mod));
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

    if (!sol_ui_system_set_keymap_override(ui, action, sequence, step_mods, step_count)) {
        fprintf(stderr, "bindings.conf: line %zu: registration failed for `%s`\n",
                line_no, action);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Surgical bindings.conf editing                                      */
/* ------------------------------------------------------------------ */

/* Upper bound on a bindings.conf that the editor will rewrite. */
#define SOL_BINDINGS_MAX_BYTES (1024u * 1024u)

/* Growable byte buffer used to assemble a rewritten bindings.conf. */
typedef struct SolConfigText {
    char  *data;
    size_t len;
    size_t cap;
} SolConfigText;

/*
 * Append bytes to a text buffer, growing it geometrically.
 *
 * text  Destination buffer.
 * src   Bytes to append.
 * len   Number of bytes.
 * Returns false on allocation failure (the buffer is left intact).
 */
static bool sol_config_text_append(SolConfigText *text, const char *src, size_t len)
{
    if (text->len + len + 1u > text->cap) {
        size_t cap = text->cap ? text->cap : 1024u;
        while (cap < text->len + len + 1u) cap *= 2u;
        char *grown = (char *)realloc(text->data, cap);
        if (!grown) return false;
        text->data = grown;
        text->cap  = cap;
    }
    memcpy(text->data + text->len, src, len);
    text->len += len;
    text->data[text->len] = '\0';
    return true;
}

/*
 * Read bindings.conf into a heap string. A missing or empty file yields
 * the default template, matching what the loader would materialise.
 *
 * path  Absolute path to bindings.conf.
 * Returns heap string the caller frees, or NULL on I/O or size error.
 */
static char *sol_config_read_bindings_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errno != ENOENT) {
            fprintf(stderr, "sol: cannot open '%s': %s\n", path, strerror(errno));
            return NULL;
        }
        const size_t len = strlen(SOL_DEFAULT_BINDINGS_CONF);
        char *copy = (char *)malloc(len + 1u);
        if (copy) memcpy(copy, SOL_DEFAULT_BINDINGS_CONF, len + 1u);
        return copy;
    }

    SolConfigText text = {0};
    char chunk[4096];
    size_t got = 0u;
    bool ok = true;
    while ((got = fread(chunk, 1u, sizeof(chunk), fp)) > 0u) {
        if (text.len + got > SOL_BINDINGS_MAX_BYTES ||
            !sol_config_text_append(&text, chunk, got)) {
            ok = false;
            break;
        }
    }
    if (ferror(fp)) ok = false;
    fclose(fp);
    if (!ok) {
        fprintf(stderr, "sol: cannot read '%s'\n", path);
        free(text.data);
        return NULL;
    }
    if (text.len == 0u) {
        free(text.data);
        const size_t len = strlen(SOL_DEFAULT_BINDINGS_CONF);
        char *copy = (char *)malloc(len + 1u);
        if (copy) memcpy(copy, SOL_DEFAULT_BINDINGS_CONF, len + 1u);
        return copy;
    }
    return text.data;
}

/* Which directive lines a rewrite pass replaces. */
typedef enum SolConfigDirective {
    SOL_CONFIG_DIRECTIVE_ACTION,   /* bind ... <action> / unbind <action> */
    SOL_CONFIG_DIRECTIVE_LEADER,   /* leader <modifier>                   */
} SolConfigDirective;

/*
 * Rewrite bindings.conf so exactly one line of the given directive kind
 * remains, replaced by `replacement` (NULL drops them all). Unrelated
 * lines, comments included, are preserved byte for byte. When nothing
 * matched, the replacement is appended — or, for the leader directive,
 * inserted before the first bind/unbind line it must precede.
 *
 * kind         Directive kind to match.
 * action       Action name for SOL_CONFIG_DIRECTIVE_ACTION; ignored otherwise.
 * replacement  Full replacement line without newline, or NULL.
 * rebase_from  For the leader rewrite: old leader name whose literal use
 *              as a bind line's second token is rewritten to `L`, keeping
 *              those lines valid under the new leader. NULL otherwise.
 * Returns true when the file was written.
 */
static bool sol_config_rewrite_bindings(SolConfigDirective kind,
                                        const char        *action,
                                        const char        *replacement,
                                        const char        *rebase_from)
{
    char *path = sol_config_path("bindings.conf");
    if (!path) return false;
    char *src = sol_config_read_bindings_text(path);
    if (!src) {
        free(path);
        return false;
    }

    SolConfigText out = {0};
    bool ok = true;
    bool replaced = (replacement == NULL);

    const char *cursor = src;
    while (ok && *cursor) {
        const char *eol  = strchr(cursor, '\n');
        const size_t len = eol ? (size_t)(eol - cursor) + 1u : strlen(cursor);

        char line[1024];
        char *tokens[8 + SOL_UI_MAX_FLOW_SEQUENCE_LEN];
        size_t n = 0u;
        if (len < sizeof(line)) {
            memcpy(line, cursor, len);
            line[len] = '\0';
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            n = sol_tokenise_inplace(line, tokens, sizeof(tokens) / sizeof(tokens[0]));
        }

        const bool is_bind   = n >= 4u && sol_streq_ci(tokens[0], "bind");
        const bool is_unbind = n >= 2u && sol_streq_ci(tokens[0], "unbind");
        bool matches = false;
        if (kind == SOL_CONFIG_DIRECTIVE_ACTION) {
            matches = (is_bind && strcmp(tokens[n - 1u], action) == 0) ||
                      (is_unbind && strcmp(tokens[1], action) == 0);
        } else {
            matches = n >= 1u && sol_streq_ci(tokens[0], "leader");
            if (!matches && !replaced && (is_bind || is_unbind)) {
                ok = sol_config_text_append(&out, replacement, strlen(replacement)) &&
                     sol_config_text_append(&out, "\n", 1u);
                replaced = true;
            }
        }

        if (!ok) break;
        if (matches) {
            if (!replaced) {
                ok = sol_config_text_append(&out, replacement, strlen(replacement)) &&
                     sol_config_text_append(&out, "\n", 1u);
                replaced = true;
            }
        } else if (rebase_from && is_bind && sol_streq_ci(tokens[1], rebase_from) &&
                   !sol_streq_ci(tokens[1], "L")) {
            ok = sol_config_text_append(&out, "bind L", 6u);
            for (size_t i = 2u; ok && i < n; ++i) {
                ok = sol_config_text_append(&out, " ", 1u) &&
                     sol_config_text_append(&out, tokens[i], strlen(tokens[i]));
            }
            ok = ok && sol_config_text_append(&out, "\n", 1u);
        } else {
            ok = sol_config_text_append(&out, cursor, len);
        }
        cursor += len;
    }

    if (ok && !replaced) {
        if (out.len > 0u && out.data[out.len - 1u] != '\n') {
            ok = sol_config_text_append(&out, "\n", 1u);
        }
        ok = ok && sol_config_text_append(&out, replacement, strlen(replacement)) &&
             sol_config_text_append(&out, "\n", 1u);
    }

    if (ok) ok = sol_config_write_atomic(path, out.data ? out.data : "", out.len);
    free(out.data);
    free(src);
    free(path);
    return ok;
}

/*
 * Persist the chord for one action in bindings.conf, replacing any
 * existing bind/unbind line for it.
 *
 * action     Dotted action name (no whitespace or '#').
 * sequence   Key steps after the leader; NULL or length 0 writes
 *            `unbind <action>` so the built-in default stays removed.
 * modifiers  Per-step modifier masks (may be NULL).
 * length     Number of steps.
 * Returns true when the file was written.
 */
bool sol_config_save_binding(const char            *action,
                             const SolKeyCode      *sequence,
                             const SolModifierMask *modifiers,
                             size_t                 length)
{
    if (!action || !action[0] || strpbrk(action, " \t\r\n#")) return false;
    if (length > SOL_UI_MAX_FLOW_SEQUENCE_LEN) return false;

    char line[512];
    int n = 0;
    if (sequence && length > 0u) {
        char chord[256];
        if (!sol_config_format_chord(sequence, modifiers, length, chord, sizeof(chord))) {
            return false;
        }
        n = snprintf(line, sizeof(line), "bind L %-16s %s", chord, action);
    } else {
        n = snprintf(line, sizeof(line), "unbind %s", action);
    }
    if (n < 0 || (size_t)n >= sizeof(line)) return false;
    return sol_config_rewrite_bindings(SOL_CONFIG_DIRECTIVE_ACTION, action, line, NULL);
}

/*
 * Persist the leader modifier in bindings.conf. Bind lines spelling the
 * previous leader literally are rewritten to the `L` placeholder so they
 * remain valid under the new leader.
 *
 * previous  Leader in effect before the change.
 * leader    New leader modifier.
 * Returns true when the file was written.
 */
bool sol_config_save_leader(SolModifierMask previous, SolModifierMask leader)
{
    char line[32];
    snprintf(line, sizeof(line), "leader %s", sol_config_modifier_name(leader));
    return sol_config_rewrite_bindings(SOL_CONFIG_DIRECTIVE_LEADER, NULL, line,
                                       sol_config_modifier_name(previous));
}

/*
 * Enumerate the bind lines of Sol's default bindings template.
 *
 * out       Receives up to capacity entries (may be NULL to count).
 * capacity  Capacity of out.
 * Returns   Total number of default bindings (may exceed capacity).
 */
size_t sol_config_default_bindings(SolConfigBinding *out, size_t capacity)
{
    size_t count = 0u;
    const char *cursor = SOL_DEFAULT_BINDINGS_CONF;
    while (*cursor) {
        const char *eol  = strchr(cursor, '\n');
        const size_t len = eol ? (size_t)(eol - cursor) : strlen(cursor);
        char line[256];
        if (len < sizeof(line)) {
            memcpy(line, cursor, len);
            line[len] = '\0';
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            char *tokens[8 + SOL_UI_MAX_FLOW_SEQUENCE_LEN];
            const size_t n = sol_tokenise_inplace(line, tokens,
                                                  sizeof(tokens) / sizeof(tokens[0]));
            if (n >= 4u && sol_streq_ci(tokens[0], "bind")) {
                if (out && count < capacity) {
                    SolConfigBinding *b = &out[count];
                    snprintf(b->action, sizeof(b->action), "%s", tokens[n - 1u]);
                    b->chord[0] = '\0';
                    size_t used = 0u;
                    for (size_t i = 2u; i + 1u < n && used < sizeof(b->chord); ++i) {
                        const int w = snprintf(b->chord + used, sizeof(b->chord) - used,
                                               "%s%s", i > 2u ? " " : "", tokens[i]);
                        if (w < 0) break;
                        used += (size_t)w;
                    }
                }
                ++count;
            }
        }
        cursor += len;
        if (*cursor == '\n') ++cursor;
    }
    return count;
}

/*
 * Replace bindings.conf with Sol's default template.
 *
 * Returns true when the file was written.
 */
bool sol_config_reset_bindings(void)
{
    char *path = sol_config_path("bindings.conf");
    if (!path) return false;
    const bool ok = sol_write_default_bindings(path);
    free(path);
    return ok;
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

    /* Auto-emit defaults the first time. The file is then user-editable.
       A zero-length file counts as absent: the only ways to get one are an
       interrupted first-launch write or a truncated save, and treating it
       as a real (bindingless) config would leave the user with no working
       keys and no hint as to why. */
    struct stat st;
    const bool stat_ok = (stat(path, &st) == 0);
    if (!stat_ok && errno != ENOENT) {
        fprintf(stderr, "sol: stat('%s'): %s\n", path, strerror(errno));
        free(path);
        return -1;
    }
    if (!stat_ok || st.st_size == 0) {
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

    sol_ui_system_set_leader_modifier(ui, SOL_MOD_CTRL);

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

        if (sol_streq_ci(tokens[0], "unbind")) {
            if (n != 2u) {
                fprintf(stderr, "bindings.conf: line %zu: `unbind` takes one action\n",
                        line_no);
            } else {
                (void)sol_ui_system_set_keymap_override(ui, tokens[1], NULL, NULL, 0u);
            }
            continue;
        }

        fprintf(stderr, "bindings.conf: line %zu: unknown directive `%s`\n",
                line_no, tokens[0]);
    }

    fclose(fp);

    /* A file that parses but binds nothing leaves the editor with no
       keyboard commands at all. That is indistinguishable from a broken
       install unless it is said out loud, so say it and name the file. */
    if (registered == 0) {
        fprintf(stderr,
                "sol: no key bindings registered from '%s' — every keyboard "
                "command is disabled. Delete the file to regenerate the "
                "defaults.\n", path);
    }

    free(path);
    return registered;
}
