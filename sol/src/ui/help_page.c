// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* help_page.c — generated help document (command cheatsheet + app guide).
 *
 * The cheatsheet is rendered from the live command-flow registry, so it
 * always shows the user's effective leader and chords (bindings.conf
 * overrides, plugin commands) rather than a static copy of the defaults.
 * Output is Markdown, displayed by the host in a read-only text buffer.
 */

#include "sol_ui_internal.h"
#include "sol_config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Growable output string. `failed` latches the first allocation error so
   callers can append unconditionally and check once at the end. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   failed;
} HelpDoc;

/* Known command categories: action prefix, heading, display order. Any
   other prefix (plugin commands) gets a capitalised heading after these. */
typedef struct {
    const char *prefix;
    const char *title;
} HelpCategory;

static const HelpCategory HELP_CATEGORIES[] = {
    { "buffer",   "Buffers"   },
    { "edit",     "Editing"   },
    { "pane",     "Panes"     },
    { "explorer", "Explorer"  },
    { "find",     "Search"    },
    { "terminal", "Terminal"  },
    { "project",  "Projects"  },
    { "help",     "Help"      },
};

#define HELP_CATEGORY_COUNT (sizeof(HELP_CATEGORIES) / sizeof(HELP_CATEGORIES[0]))
#define HELP_PREFIX_MAX     32u
#define HELP_CHORD_MAX      96u
#define HELP_TEXT_MAX       160u

/* One cheatsheet row, resolved from a registered flow. */
typedef struct {
    const SolCommandFlowBinding *flow;
    char prefix[HELP_PREFIX_MAX];
    char chord[HELP_CHORD_MAX];   /* "" when the command is unbound */
    int  category_rank;           /* index into HELP_CATEGORIES, or COUNT */
} HelpRow;

/**
 * Append formatted text to the document.
 *
 * @param doc Document being built.
 * @param fmt printf-style format.
 */
static void help_append(HelpDoc *doc, const char *fmt, ...)
{
    if (doc->failed) return;
    va_list args;
    va_start(args, fmt);
    va_list measure;
    va_copy(measure, args);
    const int needed = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (needed < 0) {
        doc->failed = true;
        va_end(args);
        return;
    }
    const size_t want = doc->len + (size_t)needed + 1u;
    if (want > doc->cap) {
        size_t cap = doc->cap ? doc->cap : 4096u;
        while (cap < want) cap *= 2u;
        char *grown = (char *)realloc(doc->data, cap);
        if (!grown) {
            doc->failed = true;
            va_end(args);
            return;
        }
        doc->data = grown;
        doc->cap = cap;
    }
    (void)vsnprintf(doc->data + doc->len, doc->cap - doc->len, fmt, args);
    doc->len += (size_t)needed;
    va_end(args);
}

/**
 * Copy text for a Markdown table cell, replacing characters that would
 * break the row ('|' splits cells, newlines end the row, '`' opens code).
 *
 * @param src  Source text.
 * @param dst  Destination buffer.
 * @param size Capacity of dst.
 */
static void help_cell_text(const char *src, char *dst, size_t size)
{
    size_t n = 0u;
    for (; src && *src && n + 1u < size; ++src) {
        const char c = *src;
        dst[n++] = (c == '|' || c == '`') ? '\'' : (c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[n] = '\0';
}

/**
 * Resolve a row's category from its action prefix.
 *
 * @param row Row whose prefix/category_rank are filled from row->flow.
 */
static void help_row_classify(HelpRow *row)
{
    const char *action = row->flow->action;
    const char *dot = strchr(action, '.');
    size_t len = dot ? (size_t)(dot - action) : strlen(action);
    if (len >= sizeof(row->prefix)) len = sizeof(row->prefix) - 1u;
    memcpy(row->prefix, action, len);
    row->prefix[len] = '\0';
    row->category_rank = (int)HELP_CATEGORY_COUNT;
    for (size_t i = 0u; i < HELP_CATEGORY_COUNT; ++i) {
        if (strcmp(HELP_CATEGORIES[i].prefix, row->prefix) == 0) {
            row->category_rank = (int)i;
            break;
        }
    }
}

/**
 * qsort comparator: category order, then prefix (plugin groups), bound
 * before unbound, then chord text, then action name.
 *
 * @param a First HelpRow.
 * @param b Second HelpRow.
 */
static int help_row_cmp(const void *a, const void *b)
{
    const HelpRow *ra = (const HelpRow *)a;
    const HelpRow *rb = (const HelpRow *)b;
    if (ra->category_rank != rb->category_rank)
        return ra->category_rank < rb->category_rank ? -1 : 1;
    const int prefix = strcmp(ra->prefix, rb->prefix);
    if (prefix != 0) return prefix;
    const bool ua = ra->chord[0] == '\0', ub = rb->chord[0] == '\0';
    if (ua != ub) return ua ? 1 : -1;
    const int chord = strcmp(ra->chord, rb->chord);
    if (chord != 0) return chord;
    return strcmp(ra->flow->action, rb->flow->action);
}

/**
 * Heading for a row's category: the known title, or the capitalised
 * action prefix for plugin-provided groups.
 *
 * @param row  Classified row.
 * @param buf  Destination buffer.
 * @param size Capacity of buf.
 */
static void help_category_title(const HelpRow *row, char *buf, size_t size)
{
    if (row->category_rank < (int)HELP_CATEGORY_COUNT) {
        snprintf(buf, size, "%s", HELP_CATEGORIES[row->category_rank].title);
        return;
    }
    help_cell_text(row->prefix, buf, size);
    if (buf[0]) buf[0] = (char)toupper((unsigned char)buf[0]);
}

/**
 * Description for a command: the documented built-in text, else the
 * registered label when it says more than the action name.
 *
 * @param flow Registered flow.
 * @param buf  Destination buffer (cell-safe text).
 * @param size Capacity of buf.
 */
static void help_description(const SolCommandFlowBinding *flow, char *buf, size_t size)
{
    char raw[HELP_TEXT_MAX];
    if (!sol_config_action_description(flow->action, raw, sizeof(raw))) {
        snprintf(raw, sizeof(raw), "%s",
                 flow->label[0] && strcmp(flow->label, flow->action) != 0 ? flow->label : "");
    }
    help_cell_text(raw, buf, size);
}

/**
 * Append the command cheatsheet: one table per category, generated from
 * the effective keymap.
 *
 * @param doc    Document being built.
 * @param ui     UI system owning the registry.
 * @param leader Leader modifier name ("ctrl", ...).
 */
static void help_append_cheatsheet(HelpDoc *doc, const SolUISystem *ui, const char *leader)
{
    const size_t count = ui->command_flow_count;
    if (count == 0u) {
        help_append(doc, "No commands are registered.\n\n");
        return;
    }
    HelpRow *rows = (HelpRow *)calloc(count, sizeof(HelpRow));
    if (!rows) {
        doc->failed = true;
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        HelpRow *row = &rows[i];
        row->flow = flow;
        help_row_classify(row);
        char steps[HELP_CHORD_MAX];
        if (flow->sequence_length > 0u &&
            sol_config_format_chord(flow->sequence, flow->step_modifiers,
                                    flow->sequence_length, steps, sizeof(steps))) {
            snprintf(row->chord, sizeof(row->chord), "%s %s", leader, steps);
        }
    }
    qsort(rows, count, sizeof(HelpRow), help_row_cmp);

    char current[HELP_PREFIX_MAX] = "";
    for (size_t i = 0u; i < count; ++i) {
        const HelpRow *row = &rows[i];
        if (strcmp(row->prefix, current) != 0) {
            char title[HELP_PREFIX_MAX];
            help_category_title(row, title, sizeof(title));
            help_append(doc, "%s### %s\n\n", i > 0u ? "\n" : "", title);
            help_append(doc, "| Keys | Action | Description |\n| --- | --- | --- |\n");
            snprintf(current, sizeof(current), "%s", row->prefix);
        }
        char description[HELP_TEXT_MAX];
        char action[SOL_UI_MAX_ACTION_LEN + 1u];
        help_description(row->flow, description, sizeof(description));
        help_cell_text(row->flow->action, action, sizeof(action));
        if (row->chord[0]) {
            help_append(doc, "| `%s` | `%s` | %s |\n", row->chord, action, description);
        } else {
            help_append(doc, "| unbound | `%s` | %s |\n", action, description);
        }
    }
    help_append(doc, "\n");
    free(rows);
}

char *sol_ui_system_build_help_document(const SolUISystem *ui, size_t *out_len)
{
    if (out_len) *out_len = 0u;
    if (!ui) return NULL;

    HelpDoc doc = { 0 };
    const char *leader = sol_config_modifier_name(ui->leader_modifier);

    help_append(&doc,
        "# Sol Help\n"
        "\n"
        "Sol is keyboard-first: every command is a **flow**\n"
        "started from the leader key. Your leader is `%s`.\n"
        "\n"
        "- Tap `%s` on its own to open the command popup.\n"
        "- Then type the keys it lists, one at a time.\n"
        "- Entries marked `+N` open a deeper group of commands.\n"
        "- `Esc`, or tapping `%s` again, cancels the flow.\n"
        "- This page is rebuilt from your live keymap on every open.\n"
        "\n"
        "## Command cheatsheet\n"
        "\n",
        leader, leader, leader);

    help_append_cheatsheet(&doc, ui, leader);

    help_append(&doc,
        "## Editing\n"
        "\n"
        "| Keys | Effect |\n"
        "| --- | --- |\n"
        "| `arrows` | Move the caret |\n"
        "| `shift+arrows` | Extend the selection |\n"
        "| `home` / `end` | Line start / end (`shift` selects) |\n"
        "| `backspace` / `delete` | Delete backward / forward |\n"
        "| `enter` | New line |\n"
        "\n"
        "- Click places the caret; drag selects.\n"
        "- Right-click buffers, tabs and explorer rows for more.\n"
        "- Clipboard, undo and delete commands are **Editing** flows.\n"
        "- In those flows `w` scopes to a word and `l` to a line.\n"
        "- Markdown renders inline; the caret line shows raw source.\n"
        "- Read-only pages like this one can be selected and copied.\n"
        "\n"
        "## Find in buffer\n"
        "\n"
        "`buffer.find` types its query into the status bar.\n"
        "\n"
        "| Keys | Effect |\n"
        "| --- | --- |\n"
        "| `up` / `down` | Previous / next match |\n"
        "| `enter` | Land the caret on the match |\n"
        "| `escape` | Cancel; caret returns to where it was |\n"
        "\n"
        "## Terminal\n"
        "\n"
        "- Dock it bottom or right, or float it over the workspace.\n"
        "- `Esc` closes the floating terminal.\n"
        "- Docked terminals pass `Esc` through to the shell.\n"
        "- Paste with `super+v` (macOS) or `ctrl+shift+v`.\n"
        "- Each tab starts its own shell in the project folder.\n"
        "\n"
        "## Projects\n"
        "\n"
        "- Each project has its own explorer, buffers and terminals.\n"
        "- Open projects are the tabs along the top of the window.\n"
        "- Recent folders are listed on the welcome page.\n"
        "\n"
        "## Customising\n"
        "\n"
        "- **Sol > Settings...**: Theme, Preferences, Keybindings.\n"
        "- Settings live in `~/.sol/settings.json` and apply live.\n"
        "- Panel sizes and terminal position: `~/.sol/layout`.\n"
        "- Key bindings live in `~/.sol/bindings.conf`:\n"
        "\n"
        "```\n"
        "leader ctrl                  # ctrl / alt / super / shift\n"
        "bind L b s  buffer.save      # L = leader, then step keys\n"
        "bind L b shift+s  buffer.save_all\n"
        "unbind edit.delete_line      # drop a chord, keep command\n"
        "```\n"
        "\n"
        "- Steps may carry `shift+`, `alt+` or `super+`.\n"
        "- Plugin commands bind the same way, by action name.\n");

    if (doc.failed) {
        free(doc.data);
        return NULL;
    }
    if (out_len) *out_len = doc.len;
    return doc.data;
}
