// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* file_picker.c — Implementation of sol_file_picker.h.
 *
 * Each picker owns a Ca_Window, a current-directory listing (one
 * level deep), and a click-context pool whose entries must remain
 * stable across reactive rebuilds (causality button click_data
 * pointers persist between frames).
 *
 * Listing strategy: we scan one directory level on demand. Clicking
 * a directory rescans into that directory; clicking a file (in file
 * mode) confirms the selection. Folder mode also exposes a "Select
 * This Folder" button that confirms the current directory.
 *
 * Lifetime: pickers are tracked in a static singly-linked list. The
 * host calls sol_file_picker_tick() every frame; entries whose
 * Ca_Window has been closed are destroyed and unlinked. The user
 * callback is fired exactly once — either from a confirm/cancel
 * click handler (which then closes the window) or from the tick
 * reaper (with NULL when the window was closed by other means).
 */

#include "sol_file_picker.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* Tunables                                                          */
/* ---------------------------------------------------------------- */

#define SOL_FP_INITIAL_CTX_CAP   64u
#define SOL_FP_DEFAULT_WIDTH     720
#define SOL_FP_DEFAULT_HEIGHT    520

/* ---------------------------------------------------------------- */
/* Types                                                             */
/* ---------------------------------------------------------------- */

typedef struct SolFpEntry {
    char *name;       /* basename, owned                              */
    char *full_path;  /* absolute, owned                              */
    bool  is_dir;
} SolFpEntry;

typedef struct SolFpClickCtx {
    SolFilePicker *picker;
    int            index;     /* >= 0: row index; -1: parent button   */
} SolFpClickCtx;

struct SolFilePicker {
    Ca_Window            *window;
    SolFilePickerMode     mode;
    SolFilePickerCallback callback;
    void                 *user_data;
    bool                  fired;       /* callback already invoked    */

    /* Current directory listing. */
    char         *current_dir;        /* owned                        */
    SolFpEntry   *entries;            /* owned heap array             */
    size_t        entry_count;
    size_t        entry_capacity;

    /* Reactive content host (rebuild on navigation). */
    Ca_Div       *content_host;

    /* Click-context pool — pointers must survive realloc-free across
       rebuilds, so we use a heap array that only grows. */
    SolFpClickCtx *click_ctxs;
    size_t         click_ctx_count;
    size_t         click_ctx_capacity;

    /* Intrusive list link. */
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
    if (!o) return NULL;
    memcpy(o, s, n + 1u);
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
    size_t pn = strlen(parent);
    size_t nn = strlen(name);
    bool need_sep = (pn > 0u && parent[pn - 1u] != '/');
    char *out = (char *)malloc(pn + (need_sep ? 1u : 0u) + nn + 1u);
    if (!out) return NULL;
    memcpy(out, parent, pn);
    size_t off = pn;
    if (need_sep) out[off++] = '/';
    memcpy(out + off, name, nn + 1u);
    return out;
}

static char *fp_parent_dir(const char *path)
{
    if (!path || !*path) return fp_strdup("/");
    size_t n = strlen(path);
    /* Strip trailing slashes (but keep root "/" intact). */
    while (n > 1u && path[n - 1u] == '/') --n;
    /* Find last slash in [0, n). */
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; ++i) if (path[i] == '/') last = i;
    if (last == (size_t)-1) return fp_strdup("/");
    if (last == 0u)         return fp_strdup("/");
    char *out = (char *)malloc(last + 1u);
    if (!out) return NULL;
    memcpy(out, path, last);
    out[last] = '\0';
    return out;
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
    p->entry_count = 0u;
}

static bool fp_entries_push(SolFilePicker *p, char *name, char *full, bool is_dir)
{
    if (p->entry_count == p->entry_capacity) {
        size_t nc = p->entry_capacity ? p->entry_capacity * 2u : 32u;
        SolFpEntry *na = (SolFpEntry *)realloc(p->entries, nc * sizeof(SolFpEntry));
        if (!na) return false;
        p->entries = na;
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

    DIR *d = opendir(p->current_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;     /* hide dotfiles + . / .. */

        char *full = fp_path_join(p->current_dir, name);
        if (!full) continue;

        struct stat st;
        bool is_dir = false;
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;

        char *dup_name = fp_strdup(name);
        if (!dup_name) { free(full); continue; }
        if (!fp_entries_push(p, dup_name, full, is_dir)) {
            free(dup_name);
            free(full);
            break;
        }
    }
    closedir(d);

    qsort(p->entries, p->entry_count, sizeof(SolFpEntry), fp_entry_cmp);
}

static bool fp_set_current_dir(SolFilePicker *p, const char *path)
{
    char *dup = fp_strdup(path && *path ? path : "/");
    if (!dup) return false;
    free(p->current_dir);
    p->current_dir = dup;
    fp_load_directory(p);
    return true;
}

/* ---------------------------------------------------------------- */
/* Click context pool                                                */
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
        p->click_ctxs = grown;
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

    if (ctx->index < 0) {
        /* Parent button. */
        char *parent = fp_parent_dir(p->current_dir);
        if (parent) {
            fp_set_current_dir(p, parent);
            free(parent);
            if (p->content_host) ca_div_invalidate(p->content_host);
        }
        return;
    }

    if ((size_t)ctx->index >= p->entry_count) return;
    const SolFpEntry *e = &p->entries[ctx->index];

    if (e->is_dir) {
        /* Navigate into directory regardless of mode. */
        char *into = fp_strdup(e->full_path);
        if (into) {
            fp_set_current_dir(p, into);
            free(into);
            if (p->content_host) ca_div_invalidate(p->content_host);
        }
    } else if (p->mode == SOL_FILE_PICKER_FILE) {
        fp_confirm(p, e->full_path);
    }
    /* In folder mode, clicking a file is a no-op. */
}

static void fp_on_select_folder(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    fp_confirm(p, p->current_dir);
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
    ctx->index  = (int)index;

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "fp-row",
        .direction  = CA_HORIZONTAL,
        .on_click   = fp_on_row_click,
        .click_data = ctx,
    });

    /* Nerd Font glyphs: folder / file. */
    ca_text(&(Ca_TextDesc){
        .text  = e->is_dir ? "\xef\x81\xbb"   /*  folder */
                           : "\xef\x85\x9b",  /*  file   */
        .style = e->is_dir ? "fp-glyph fp-glyph-dir" : "fp-glyph",
    });
    ca_text(&(Ca_TextDesc){
        .text  = e->name,
        .style = e->is_dir ? "fp-name fp-name-dir" : "fp-name",
    });

    ca_btn_end();
}

static void fp_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;

    /* Reset click pool for this rebuild. The pool is grown but never
       shrunk so any pointer we hand to causality remains valid until
       the picker itself is destroyed. */
    p->click_ctx_count = 0u;

    /* Header: current path + parent button. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "fp-header",
    });

    {
        SolFpClickCtx *up_ctx = fp_acquire_ctx(p);
        if (up_ctx) {
            up_ctx->picker = p;
            up_ctx->index  = -1;
            ca_btn_begin(&(Ca_BtnDesc){
                .style      = "fp-up",
                .direction  = CA_HORIZONTAL,
                .on_click   = fp_on_row_click,
                .click_data = up_ctx,
            });
            /*  arrow-up (Nerd Font / FontAwesome). */
            ca_text(&(Ca_TextDesc){
                .text = "\xef\x81\xa2",
                .style = "fp-up-text",
            });
            ca_btn_end();
        }
    }

    ca_text(&(Ca_TextDesc){
        .text  = p->current_dir ? p->current_dir : "/",
        .style = "fp-path",
    });

    ca_div_end();   /* fp-header */

    /* Body: entries. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "fp-list",
    });

    if (p->entry_count == 0u) {
        ca_text(&(Ca_TextDesc){
            .text  = "(empty)",
            .style = "fp-empty",
        });
    } else {
        for (size_t i = 0; i < p->entry_count; ++i) {
            fp_render_row(p, i);
        }
    }

    ca_div_end();   /* fp-list */

    /* Footer: action buttons. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "fp-footer",
    });

    if (p->mode == SOL_FILE_PICKER_FOLDER) {
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-action fp-action-primary",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_select_folder,
            .click_data = p,
        });
        ca_text(&(Ca_TextDesc){
            .text  = "Select This Folder",
            .style = "fp-action-text",
        });
        ca_btn_end();
    }

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "fp-action",
        .direction  = CA_HORIZONTAL,
        .on_click   = fp_on_cancel,
        .click_data = p,
    });
    ca_text(&(Ca_TextDesc){
        .text  = "Cancel",
        .style = "fp-action-text",
    });
    ca_btn_end();

    ca_div_end();   /* fp-footer */
}

static void fp_build_layout(SolFilePicker *p)
{
    ca_ui_begin(p->window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "fp-root",
    });

    p->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "fp-host",
    });
    /* set_builder runs the builder synchronously on registration and on
       every invalidate; it clears children before each run. Do NOT also
       call the builder explicitly here — that would emit content twice. */
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
    /* Window is owned by causality; if still open, request close. */
    if (p->window && ca_window_is_open(p->window)) {
        ca_window_close(p->window);
    }
    fp_entries_clear(p);
    free(p->entries);
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
        if (getcwd(cwd_buf, sizeof(cwd_buf))) start = cwd_buf;
        else                                   start = "/";
    }
    /* If the path is a regular file, fall back to its parent. */
    {
        struct stat st;
        if (stat(start, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char *parent = fp_parent_dir(start);
            if (parent) {
                if (!fp_set_current_dir(p, parent)) {
                    free(parent);
                    fp_destroy(p);
                    return NULL;
                }
                free(parent);
            }
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
    if (!p->window) {
        fp_destroy(p);
        return NULL;
    }

    fp_build_layout(p);

    /* Link into the global registry. */
    p->next    = g_pickers;
    g_pickers  = p;

    return p;
}

void sol_file_picker_tick(void)
{
    SolFilePicker **link = &g_pickers;
    while (*link) {
        SolFilePicker *p = *link;
        if (!p->window || !ca_window_is_open(p->window)) {
            /* Window has been closed (by user X, by ca_window_close, or
               by destroy). Fire the callback with NULL if we never
               confirmed/cancelled, then unlink and destroy. */
            fp_fire(p, NULL);
            *link = p->next;
            /* The window itself is owned by causality and is already
               in teardown — null our handle so fp_destroy doesn't try
               to close it again. */
            p->window = NULL;
            fp_destroy(p);
            continue;
        }
        link = &p->next;
    }
}
