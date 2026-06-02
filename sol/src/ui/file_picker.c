// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* file_picker.c — Redesigned sol_file_picker implementation.
 *
 * A native-window file/folder picker with:
 *   - Breadcrumb toolbar: click any ancestor path segment to jump there.
 *   - Scrollable file list with type-specific Nerd Font icons matching
 *     the main file tree panel.
 *   - Consistent look/feel: same colors, row heights, and hover styles
 *     as the rest of the editor.
 *
 * Architecture:
 *   - Click contexts are allocated from a monotonically growing pool so
 *     pointers handed to causality survive reactive rebuilds.
 *   - Breadcrumb path segments (crumb_paths[]) are rebuilt whenever the
 *     current directory changes, alongside the directory scan.
 *   - The reactive content host is invalidated on every navigation.
 */

#include "sol_file_picker.h"

#include "sol_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Tunables                                                          */
/* ---------------------------------------------------------------- */

#define SOL_FP_INITIAL_CTX_CAP   64u
#define SOL_FP_DEFAULT_WIDTH     760
#define SOL_FP_DEFAULT_HEIGHT    520

/* ---------------------------------------------------------------- */
/* Nerd Font glyphs (same as file_tree_panel.c)                     */
/* ---------------------------------------------------------------- */

#define FP_ICON_DIR_CLOSED   "\xef\x81\xbb"  /* U+F07B  fa-folder          */
#define FP_ICON_FILE_GENERIC "\xef\x80\x96"  /* U+F016  fa-file-o          */
#define FP_ICON_FILE_C       "\xee\x98\x9e"  /* U+E61E  nf-dev-c           */
#define FP_ICON_FILE_CPP     "\xee\x98\x9d"  /* U+E61D  nf-dev-cplusplus   */
#define FP_ICON_FILE_PY      "\xee\x98\x86"  /* U+E606  nf-dev-python      */
#define FP_ICON_FILE_JS      "\xee\x98\x8c"  /* U+E60C  nf-dev-javascript  */
#define FP_ICON_FILE_TS      "\xee\x98\xa8"  /* U+E628  nf-seti-typescript */
#define FP_ICON_FILE_HTML    "\xee\x98\x8e"  /* U+E60E  nf-dev-html5       */
#define FP_ICON_FILE_CSS     "\xee\x98\x8a"  /* U+E60A  nf-dev-css3        */
#define FP_ICON_FILE_JSON    "\xee\x98\x8b"  /* U+E60B  nf-dev-json        */
#define FP_ICON_FILE_MD      "\xef\x92\x8a"  /* U+F48A  nf-fa-markdown     */
#define FP_ICON_FILE_COG     "\xef\x80\x93"  /* U+F013  fa-cog (cmake)     */

/* ---------------------------------------------------------------- */
/* Types                                                             */
/* ---------------------------------------------------------------- */

/* Kind tag for the unified click-context pool. */
#define FP_CTX_ENTRY  0   /* index = row in entries[]        */
#define FP_CTX_CRUMB  1   /* index = position in crumb_paths */

typedef struct SolFpEntry {
    char *name;       /* basename, owned */
    char *full_path;  /* absolute,  owned */
    bool  is_dir;
} SolFpEntry;

typedef struct SolFpClickCtx {
    SolFilePicker *picker;
    int            kind;
    int            index;
} SolFpClickCtx;

struct SolFilePicker {
    Ca_Window            *window;
    SolFilePickerMode     mode;
    SolFilePickerCallback callback;
    void                 *user_data;
    bool                  fired;

    /* Current directory and its flat listing. */
    char       *current_dir;
    SolFpEntry *entries;
    size_t      entry_count;
    size_t      entry_capacity;

    /* Breadcrumb segments.
       crumb_paths[i] = null-terminated absolute path up to segment i.
       Rebuilt by fp_build_crumbs() whenever current_dir changes.    */
    char  **crumb_paths;
    size_t  crumb_count;
    size_t  crumb_capacity;

    /* Reactive content host (rebuild on navigation). */
    Ca_Div *content_host;

    /* Click-context pool — grows but never shrinks so pointers given
       to causality remain valid until the picker is destroyed.       */
    SolFpClickCtx *click_ctxs;
    size_t         click_ctx_count;
    size_t         click_ctx_capacity;

    SolFilePicker *next;
};

/* Singly-linked head of all live pickers. */
static SolFilePicker *g_pickers = NULL;

/* ---------------------------------------------------------------- */
/* Small helpers                                                     */
/* ---------------------------------------------------------------- */

static char *fp_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (o) memcpy(o, s, n + 1u);
    return o;
}

static int fp_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a; if (ca >= 'A' && ca <= 'Z') ca += 32;
        int cb = *b; if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int fp_entry_cmp(const void *a, const void *b)
{
    const SolFpEntry *x = (const SolFpEntry *)a;
    const SolFpEntry *y = (const SolFpEntry *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return fp_casecmp(x->name, y->name);
}

static char *fp_path_join(const char *parent, const char *name)
{
    return sol_platform_path_join(parent, name);
}

static char *fp_parent_dir(const char *path)
{
    if (!path || !*path) return fp_strdup(".");

    size_t n = strlen(path);
    while (n > 1u && sol_platform_is_path_separator(path[n - 1u])) --n;

    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; ++i)
        if (sol_platform_is_path_separator(path[i])) last = i;

    if (last == (size_t)-1) return fp_strdup(".");

    /* Windows drive root: "C:\" */
    if (last == 2u && path[1] == ':' && sol_platform_is_path_separator(path[2])) {
        char *out = (char *)malloc(4u);
        if (!out) return NULL;
        out[0] = path[0]; out[1] = path[1]; out[2] = path[2]; out[3] = '\0';
        return out;
    }
    if (last == 0u) return fp_strdup(path[0] == '\\' ? "\\" : "/");

    char *out = (char *)malloc(last + 1u);
    if (!out) return NULL;
    memcpy(out, path, last);
    out[last] = '\0';
    return out;
}

/* Return a pointer into crumb_path at the start of its last path
   segment — used as the display label for each breadcrumb button.  */
static const char *fp_crumb_label(const char *crumb_path)
{
    if (!crumb_path || !crumb_path[0]) return "";
    size_t n = strlen(crumb_path);
    /* Strip trailing separators (but keep at least one character). */
    size_t end = n;
    while (end > 1u && sol_platform_is_path_separator(crumb_path[end - 1u])) --end;
    /* Walk backward to the preceding separator. */
    for (size_t i = end; i > 0; --i) {
        if (sol_platform_is_path_separator(crumb_path[i - 1u])) {
            const char *label = crumb_path + i;
            return label[0] ? label : "/";
        }
    }
    return crumb_path;
}

/* ---------------------------------------------------------------- */
/* File-type icon selector (mirrors file_tree_panel.c)              */
/* ---------------------------------------------------------------- */

static const char *fp_file_icon(const char *name, const char **out_style)
{
    if (name && strncmp(name, "CMakeLists", 10) == 0) {
        *out_style = "tree-icon tree-icon-cmake"; return FP_ICON_FILE_COG;
    }
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot) {
        if (fp_casecmp(dot, ".c")     == 0) { *out_style = "tree-icon tree-icon-c";     return FP_ICON_FILE_C;   }
        if (fp_casecmp(dot, ".h")     == 0) { *out_style = "tree-icon tree-icon-h";     return FP_ICON_FILE_C;   }
        if (fp_casecmp(dot, ".cpp")   == 0 ||
            fp_casecmp(dot, ".cc")    == 0) { *out_style = "tree-icon tree-icon-c";     return FP_ICON_FILE_CPP; }
        if (fp_casecmp(dot, ".hpp")   == 0 ||
            fp_casecmp(dot, ".hh")    == 0) { *out_style = "tree-icon tree-icon-h";     return FP_ICON_FILE_CPP; }
        if (fp_casecmp(dot, ".py")    == 0) { *out_style = "tree-icon tree-icon-py";    return FP_ICON_FILE_PY;  }
        if (fp_casecmp(dot, ".js")    == 0) { *out_style = "tree-icon tree-icon-js";    return FP_ICON_FILE_JS;  }
        if (fp_casecmp(dot, ".ts")    == 0) { *out_style = "tree-icon tree-icon-ts";    return FP_ICON_FILE_TS;  }
        if (fp_casecmp(dot, ".json")  == 0) { *out_style = "tree-icon tree-icon-json";  return FP_ICON_FILE_JSON;}
        if (fp_casecmp(dot, ".html")  == 0 ||
            fp_casecmp(dot, ".htm")   == 0) { *out_style = "tree-icon tree-icon-html";  return FP_ICON_FILE_HTML;}
        if (fp_casecmp(dot, ".css")   == 0) { *out_style = "tree-icon tree-icon-css";   return FP_ICON_FILE_CSS; }
        if (fp_casecmp(dot, ".md")    == 0) { *out_style = "tree-icon tree-icon-md";    return FP_ICON_FILE_MD;  }
        if (fp_casecmp(dot, ".cmake") == 0) { *out_style = "tree-icon tree-icon-cmake"; return FP_ICON_FILE_COG; }
    }
    *out_style = "tree-icon tree-icon-file";
    return FP_ICON_FILE_GENERIC;
}

/* ---------------------------------------------------------------- */
/* Breadcrumb path building                                          */
/* ---------------------------------------------------------------- */

static void fp_crumbs_clear(SolFilePicker *p)
{
    for (size_t i = 0; i < p->crumb_count; ++i) free(p->crumb_paths[i]);
    p->crumb_count = 0;
}

static bool fp_crumb_push_len(SolFilePicker *p, const char *path, size_t len)
{
    if (p->crumb_count == p->crumb_capacity) {
        size_t nc = p->crumb_capacity ? p->crumb_capacity * 2u : 8u;
        char **na = (char **)realloc(p->crumb_paths, nc * sizeof(char *));
        if (!na) return false;
        p->crumb_paths    = na;
        p->crumb_capacity = nc;
    }
    char *s = (char *)malloc(len + 1u);
    if (!s) return false;
    memcpy(s, path, len);
    s[len] = '\0';
    p->crumb_paths[p->crumb_count++] = s;
    return true;
}

/* Split current_dir into prefix paths for each segment.
   E.g. "/Users/duke/Code" → ["/", "/Users", "/Users/duke",
                               "/Users/duke/Code"].              */
static void fp_build_crumbs(SolFilePicker *p)
{
    fp_crumbs_clear(p);

    const char *path = p->current_dir;
    if (!path || !*path) return;

    size_t n         = strlen(path);
    size_t effective = n;
    /* Strip trailing separators (preserving lone "/" or "C:\"). */
    while (effective > 1u && sol_platform_is_path_separator(path[effective - 1u]))
        --effective;

    size_t pos = 0u;

    /* Root component. */
    if (sol_platform_is_path_separator(path[0])) {
        fp_crumb_push_len(p, path, 1u);  /* "/" */
        pos = 1u;
    }
#ifdef _WIN32
    else if (n >= 2u &&
             ((path[0] >= 'A' && path[0] <= 'Z') ||
              (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':') {
        size_t root_len = 2u;
        if (root_len < n && sol_platform_is_path_separator(path[root_len]))
            ++root_len;
        fp_crumb_push_len(p, path, root_len);
        pos = root_len;
    }
#endif

    /* Skip extra separators immediately after root. */
    while (pos < effective && sol_platform_is_path_separator(path[pos])) ++pos;

    /* Remaining path segments. */
    while (pos < effective) {
        size_t seg_start = pos;
        while (pos < effective && !sol_platform_is_path_separator(path[pos])) ++pos;
        if (pos > seg_start) fp_crumb_push_len(p, path, pos);
        while (pos < effective && sol_platform_is_path_separator(path[pos])) ++pos;
    }
}

/* ---------------------------------------------------------------- */
/* Listing                                                           */
/* ---------------------------------------------------------------- */

static void fp_entries_clear(SolFilePicker *p)
{
    for (size_t i = 0; i < p->entry_count; ++i) {
        free(p->entries[i].name);
        free(p->entries[i].full_path);
    }
    p->entry_count = 0;
}

static bool fp_entries_push(SolFilePicker *p, char *name, char *full, bool is_dir)
{
    if (p->entry_count == p->entry_capacity) {
        size_t nc = p->entry_capacity ? p->entry_capacity * 2u : 32u;
        SolFpEntry *na = (SolFpEntry *)realloc(p->entries, nc * sizeof(SolFpEntry));
        if (!na) return false;
        p->entries        = na;
        p->entry_capacity = nc;
    }
    p->entries[p->entry_count].name      = name;
    p->entries[p->entry_count].full_path = full;
    p->entries[p->entry_count].is_dir    = is_dir;
    p->entry_count++;
    return true;
}

static void fp_load_directory(SolFilePicker *p)
{
    fp_entries_clear(p);
    if (!p->current_dir) return;

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, p->current_dir)) return;

    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        if (entry.name[0] == '.') continue;   /* skip dotfiles, . and .. */
        char *full = fp_path_join(p->current_dir, entry.name);
        if (!full) continue;
        char *name = fp_strdup(entry.name);
        if (!name) { free(full); continue; }
        if (!fp_entries_push(p, name, full, entry.is_directory)) {
            free(name); free(full); break;
        }
    }
    sol_platform_dir_close(&iter);
    qsort(p->entries, p->entry_count, sizeof(SolFpEntry), fp_entry_cmp);
}

static bool fp_set_current_dir(SolFilePicker *p, const char *path)
{
    char *dup = fp_strdup(path && *path ? path : ".");
    if (!dup) return false;
    free(p->current_dir);
    p->current_dir = dup;
    fp_load_directory(p);
    fp_build_crumbs(p);
    return true;
}

/* ---------------------------------------------------------------- */
/* Click-context pool                                                */
/* ---------------------------------------------------------------- */

static SolFpClickCtx *fp_acquire_ctx(SolFilePicker *p)
{
    if (p->click_ctx_count == p->click_ctx_capacity) {
        size_t nc = p->click_ctx_capacity
                        ? p->click_ctx_capacity * 2u
                        : SOL_FP_INITIAL_CTX_CAP;
        SolFpClickCtx *grown = (SolFpClickCtx *)realloc(
            p->click_ctxs, nc * sizeof(SolFpClickCtx));
        if (!grown) return NULL;
        p->click_ctxs         = grown;
        p->click_ctx_capacity = nc;
    }
    return &p->click_ctxs[p->click_ctx_count++];
}

/* ---------------------------------------------------------------- */
/* Confirmation / cancellation                                       */
/* ---------------------------------------------------------------- */

static void fp_fire(SolFilePicker *p, const char *path)
{
    if (p->fired) return;
    p->fired = true;
    if (p->callback) p->callback(path, p->user_data);
}

static void fp_confirm(SolFilePicker *p, const char *path)
{
    fp_fire(p, path);
    if (p->window) ca_window_close(p->window);
}

static void fp_cancel(SolFilePicker *p)
{
    fp_fire(p, NULL);
    if (p->window) ca_window_close(p->window);
}

/* ---------------------------------------------------------------- */
/* Click handlers                                                    */
/* ---------------------------------------------------------------- */

static void fp_on_row_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFpClickCtx *ctx = (SolFpClickCtx *)user_data;
    if (!ctx || !ctx->picker) return;
    SolFilePicker *p = ctx->picker;
    if ((size_t)ctx->index >= p->entry_count) return;

    const SolFpEntry *e = &p->entries[ctx->index];
    if (e->is_dir) {
        char *into = fp_strdup(e->full_path);
        if (into) {
            fp_set_current_dir(p, into);
            free(into);
            if (p->content_host) ca_div_invalidate(p->content_host);
        }
    } else if (p->mode == SOL_FILE_PICKER_FILE) {
        fp_confirm(p, e->full_path);
    }
    /* Folder mode: clicking a file is a no-op. */
}

static void fp_on_crumb_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFpClickCtx *ctx = (SolFpClickCtx *)user_data;
    if (!ctx || !ctx->picker) return;
    SolFilePicker *p = ctx->picker;
    if (ctx->index < 0 || (size_t)ctx->index >= p->crumb_count) return;

    const char *target = p->crumb_paths[ctx->index];
    if (p->current_dir && strcmp(p->current_dir, target) == 0) return;
    fp_set_current_dir(p, target);
    if (p->content_host) ca_div_invalidate(p->content_host);
}

static void fp_on_up_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    char *parent = fp_parent_dir(p->current_dir);
    if (parent) {
        if (!p->current_dir || strcmp(parent, p->current_dir) != 0) {
            fp_set_current_dir(p, parent);
            if (p->content_host) ca_div_invalidate(p->content_host);
        }
        free(parent);
    }
}

static void fp_on_select_folder(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (p) fp_confirm(p, p->current_dir);
}

static void fp_on_cancel(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (p) fp_cancel(p);
}

/* ---------------------------------------------------------------- */
/* Rendering                                                         */
/* ---------------------------------------------------------------- */

static void fp_render_row(SolFilePicker *p, size_t index)
{
    const SolFpEntry *e = &p->entries[index];

    SolFpClickCtx *ctx = fp_acquire_ctx(p);
    if (!ctx) return;
    ctx->picker = p;
    ctx->kind   = FP_CTX_ENTRY;
    ctx->index  = (int)index;

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "fp-row",
        .direction  = CA_HORIZONTAL,
        .on_click   = fp_on_row_click,
        .click_data = ctx,
    });

    if (e->is_dir) {
        ca_text(&(Ca_TextDesc){ .text = FP_ICON_DIR_CLOSED,
                                .style = "tree-icon tree-icon-dir-closed" });
        ca_text(&(Ca_TextDesc){ .text = e->name,
                                .style = "fp-row-name fp-row-name-dir" });
    } else {
        const char *icon_style;
        const char *icon = fp_file_icon(e->name, &icon_style);
        ca_text(&(Ca_TextDesc){ .text = icon,    .style = icon_style    });
        ca_text(&(Ca_TextDesc){ .text = e->name, .style = "fp-row-name" });
    }

    ca_btn_end();
}

static void fp_render_breadcrumb(SolFilePicker *p)
{
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "fp-breadcrumb",
    });

    for (size_t i = 0; i < p->crumb_count; ++i) {
        /* "/" separator between segments. */
        if (i > 0u) {
            ca_text(&(Ca_TextDesc){ .text = "/", .style = "fp-crumb-sep" });
        }

        SolFpClickCtx *ctx = fp_acquire_ctx(p);
        if (!ctx) continue;
        ctx->picker = p;
        ctx->kind   = FP_CTX_CRUMB;
        ctx->index  = (int)i;

        bool is_last = (i == p->crumb_count - 1u);

        ca_btn_begin(&(Ca_BtnDesc){
            .style      = is_last ? "fp-crumb-btn fp-crumb-btn-active"
                                  : "fp-crumb-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_crumb_click,
            .click_data = ctx,
        });
        ca_text(&(Ca_TextDesc){
            .text  = fp_crumb_label(p->crumb_paths[i]),
            .style = is_last ? "fp-crumb-text fp-crumb-text-active"
                             : "fp-crumb-text",
        });
        ca_btn_end();
    }

    ca_div_end();
}

static void fp_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;

    /* Reset click pool for this rebuild.  The pool only grows so all
       pointers handed to causality in prior builds stay valid.      */
    p->click_ctx_count = 0u;

    /* ── Toolbar: up button + breadcrumb ── */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "fp-toolbar" });
    {
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-up-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_up_click,
            .click_data = p,
        });
        /* U+F062  fa-arrow-up */
        ca_text(&(Ca_TextDesc){ .text = "\xef\x81\xa2", .style = "fp-up-icon" });
        ca_btn_end();

        fp_render_breadcrumb(p);
    }
    ca_div_end();

    /* ── Scrollable file list ── */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "fp-list" });
    if (p->entry_count == 0u) {
        ca_text(&(Ca_TextDesc){ .text = "(empty directory)", .style = "fp-empty" });
    } else {
        for (size_t i = 0; i < p->entry_count; ++i) fp_render_row(p, i);
    }
    ca_div_end();

    /* ── Footer ── */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "fp-footer" });
    {
        if (p->mode == SOL_FILE_PICKER_FOLDER) {
            ca_btn_begin(&(Ca_BtnDesc){
                .text       = "Select Folder",
                .style      = "fp-action-primary",
                .on_click   = fp_on_select_folder,
                .click_data = p,
            });
            ca_btn_end();
        }

        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "Cancel",
            .style      = "fp-action-cancel",
            .on_click   = fp_on_cancel,
            .click_data = p,
        });
        ca_btn_end();
    }
    ca_div_end();
}

static void fp_build_layout(SolFilePicker *p)
{
    ca_ui_begin(p->window, &(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "fp-root" });

    p->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "fp-host",
    });
    /* set_builder runs the builder once on registration and again on
       every invalidate; it clears children before each run.        */
    ca_div_set_builder(p->content_host, fp_content_builder, p);
    ca_div_end();

    ca_ui_end();
}

/* ---------------------------------------------------------------- */
/* Cleanup                                                           */
/* ---------------------------------------------------------------- */

static void fp_destroy(SolFilePicker *p)
{
    if (!p) return;
    if (p->window && ca_window_is_open(p->window))
        ca_window_close(p->window);
    fp_entries_clear(p);
    fp_crumbs_clear(p);
    free(p->entries);
    free(p->crumb_paths);
    free(p->click_ctxs);
    free(p->current_dir);
    free(p);
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

SolFilePicker *sol_file_picker_open(Ca_Instance          *instance,
                                    SolFilePickerMode     mode,
                                    const char           *initial_dir,
                                    SolFilePickerCallback on_select,
                                    void                 *user_data)
{
    if (!instance) return NULL;

    SolFilePicker *p = (SolFilePicker *)calloc(1u, sizeof(SolFilePicker));
    if (!p) return NULL;

    p->mode      = mode;
    p->callback  = on_select;
    p->user_data = user_data;

    /* Resolve initial directory. */
    char        cwd_buf[4096];
    const char *start = initial_dir;
    if (!start || !*start) {
        start = sol_platform_get_cwd(cwd_buf, sizeof(cwd_buf)) ? cwd_buf : ".";
    }
    {
        SolPathInfo info;
        if (sol_platform_get_path_info(start, &info) && !info.is_directory) {
            char *parent = fp_parent_dir(start);
            if (!parent || !fp_set_current_dir(p, parent)) {
                free(parent);
                fp_destroy(p);
                return NULL;
            }
            free(parent);
        } else if (!fp_set_current_dir(p, start)) {
            fp_destroy(p);
            return NULL;
        }
    }

    const char *title = (mode == SOL_FILE_PICKER_FOLDER)
                            ? "Open Folder" : "Open File";

    p->window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = title,
        .width  = SOL_FP_DEFAULT_WIDTH,
        .height = SOL_FP_DEFAULT_HEIGHT,
    });
    if (!p->window) { fp_destroy(p); return NULL; }

    fp_build_layout(p);

    p->next   = g_pickers;
    g_pickers = p;
    return p;
}

void sol_file_picker_tick(void)
{
    SolFilePicker **link = &g_pickers;
    while (*link) {
        SolFilePicker *p = *link;
        if (!p->window || !ca_window_is_open(p->window)) {
            /* Window closed: fire callback with NULL if never confirmed. */
            fp_fire(p, NULL);
            *link     = p->next;
            p->window = NULL;   /* causality owns the window teardown */
            fp_destroy(p);
            continue;
        }
        link = &p->next;
    }
}
