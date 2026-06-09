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
#include <sys/stat.h>
#include <time.h>

/* ---------------------------------------------------------------- */
/* Tunables                                                          */
/* ---------------------------------------------------------------- */

#define SOL_FP_INITIAL_CTX_CAP   64u
#define SOL_FP_DEFAULT_WIDTH     760
#define SOL_FP_DEFAULT_HEIGHT    520

/* ---------------------------------------------------------------- */
/* Nerd Font glyphs (same as file_tree_panel.c)                     */
/* ---------------------------------------------------------------- */

#define FP_ICON_DIR_CLOSED   CA_ICON_FA_FOLDER
#define FP_ICON_FILE_GENERIC CA_ICON_FA_FILE_O
#define FP_ICON_FILE_C       CA_ICON_NF_DEV_C
#define FP_ICON_FILE_CPP     CA_ICON_NF_DEV_CPP
#define FP_ICON_FILE_PY      CA_ICON_NF_DEV_PYTHON
#define FP_ICON_FILE_JS      CA_ICON_NF_DEV_JAVASCRIPT
#define FP_ICON_FILE_TS      CA_ICON_NF_SETI_TYPESCRIPT
#define FP_ICON_FILE_HTML    CA_ICON_NF_DEV_HTML5
#define FP_ICON_FILE_CSS     CA_ICON_NF_DEV_CSS3
#define FP_ICON_FILE_JSON    CA_ICON_NF_DEV_JSON
#define FP_ICON_FILE_MD      CA_ICON_NF_FA_MARKDOWN
#define FP_ICON_FILE_COG     CA_ICON_FA_COG
#define FP_ICON_HOME         CA_ICON_FA_HOME
#define FP_ICON_REFRESH      CA_ICON_FA_REFRESH
#define FP_ICON_ARROW_UP     CA_ICON_FA_ARROW_UP
#define FP_ICON_EYE          CA_ICON_FA_EYE
#define FP_ICON_EYE_SLASH    CA_ICON_FA_EYE_SLASH

#define FP_LABEL_CREATE_FOLDER "Create"
#define FP_LABEL_NEW_FOLDER    "+ New Folder"
#define FP_LABEL_SELECT_FOLDER "Select Folder"
#define FP_LABEL_CANCEL        "Cancel"

/* ---------------------------------------------------------------- */
/* Types                                                             */
/* ---------------------------------------------------------------- */

/* Kind tag for the unified click-context pool. */
#define FP_CTX_ENTRY  0   /* index = row in entries[]        */
#define FP_CTX_CRUMB  1   /* index = position in crumb_paths */
#define FP_CTX_SORT   2   /* index = FP_SORT_* column        */

/* Sort column identifiers. */
#define FP_SORT_NAME  0
#define FP_SORT_SIZE  1
#define FP_SORT_DATE  2

typedef struct SolFpEntry {
    char    *name;        /* basename, owned          */
    char    *full_path;   /* absolute path, owned     */
    bool     is_dir;
    uint64_t size_bytes;  /* 0 for directories        */
    int64_t  mtime;       /* Unix timestamp, 0=unavail*/
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

    /* Toolbar feature state */
    bool           show_hidden;            /* show dotfiles / hidden entries */
    bool           show_new_folder_bar;    /* inline new-folder creation bar */
    int            sort_by;               /* FP_SORT_NAME / SIZE / DATE     */
    bool           sort_asc;              /* true = ascending               */
    bool           new_folder_needs_focus; /* focus input on next build      */
    char           new_folder_buf[256];    /* folder name being typed        */
    Ca_TextInput  *new_folder_input;       /* reference for programmatic focus */

    SolFilePicker *next;
};

/* Singly-linked head of all live pickers. */
static SolFilePicker *g_pickers = NULL;

/* ---------------------------------------------------------------- */
/* Small helpers                                                     */
/* ---------------------------------------------------------------- */

/* Duplicate string s into a malloc-owned buffer, returning NULL when s is NULL. */
static char *fp_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (o) memcpy(o, s, n + 1u);
    return o;
}

/*
 * Case-insensitive string comparison (ASCII only).
 *
 * Returns Negative, zero, or positive like strcmp.
 */
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

/* Set to the current picker during qsort (synchronous, single-threaded). */
static SolFilePicker *g_sort_ctx = NULL;

/*
 * qsort comparator for SolFpEntry.  Directories always precede files; within
 * each group ordering follows the column (name/size/date) and direction
 * recorded in the global g_sort_ctx picker.
 */
static int fp_entry_cmp(const void *a, const void *b)
{
    const SolFpEntry *x = (const SolFpEntry *)a;
    const SolFpEntry *y = (const SolFpEntry *)b;
    /* Directories always sort before files. */
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    int  sort_by  = g_sort_ctx ? g_sort_ctx->sort_by  : FP_SORT_NAME;
    bool sort_asc = g_sort_ctx ? g_sort_ctx->sort_asc : true;
    int cmp = 0;
    switch (sort_by) {
        case FP_SORT_SIZE:
            cmp = (x->size_bytes < y->size_bytes) ? -1
                : (x->size_bytes > y->size_bytes) ?  1 : 0;
            break;
        case FP_SORT_DATE:
            cmp = (x->mtime < y->mtime) ? -1
                : (x->mtime > y->mtime) ?  1 : 0;
            break;
        default:
            cmp = fp_casecmp(x->name, y->name);
            break;
    }
    return sort_asc ? cmp : -cmp;
}

/* Join parent directory and child name into a new heap-allocated path string. */
static char *fp_path_join(const char *parent, const char *name)
{
    return sol_platform_path_join(parent, name);
}

/*
 * Return the parent directory of path as a new heap-allocated string.
 * Handles Unix roots ("/"), Windows drive roots ("C:\"), and trailing
 * separators.  Returns "." when there is no parent component.
 *
 * path    The filesystem path whose parent is needed.
 * Returns Heap-allocated parent path, or NULL on allocation failure.
 */
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

/* ---------------------------------------------------------------- */
/* Metadata format helpers                                          */
/* ---------------------------------------------------------------- */

/*
 * Format a file size as a human-readable string (B / KB / MB / GB).
 * Writes an empty string for directories.
 *
 * bytes   File size in bytes.
 * is_dir  When true, writes an empty string and returns immediately.
 * buf     Destination buffer.
 * bufsz   Size of the destination buffer in bytes.
 */
static void fp_format_size(uint64_t bytes, bool is_dir, char *buf, size_t bufsz)
{
    if (is_dir) { buf[0] = '\0'; return; }
    if (bytes < 1024u)
        snprintf(buf, bufsz, "%u B", (unsigned)bytes);
    else if (bytes < 1024u * 1024u)
        snprintf(buf, bufsz, "%.0f KB", (double)bytes / 1024.0);
    else if (bytes < 1024u * 1024u * 1024u)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, bufsz, "%.1f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

/*
 * Format a Unix timestamp as "Mon DD HH:MM" into buf.
 * Writes an empty string when mtime is 0 or localtime fails.
 *
 * mtime   Unix timestamp (seconds since epoch), 0 = unavailable.
 * buf     Destination buffer.
 * bufsz   Size of the destination buffer in bytes.
 */
static void fp_format_date(int64_t mtime, char *buf, size_t bufsz)
{
    if (mtime == 0) { buf[0] = '\0'; return; }
    time_t    t  = (time_t)mtime;
    struct tm *tm = localtime(&t);
    if (!tm)  { buf[0] = '\0'; return; }
    strftime(buf, bufsz, "%b %d %H:%M", tm);
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

/*
 * Select the Nerd Font icon glyph and CSS style for a given filename.
 *
 * name       The filename (basename) to inspect.
 * out_style  Receives the CSS class string for the icon element.
 * Returns    The icon glyph string constant.
 */
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

/* Free all breadcrumb path strings and reset the crumb count to zero. */
static void fp_crumbs_clear(SolFilePicker *p)
{
    for (size_t i = 0; i < p->crumb_count; ++i) free(p->crumb_paths[i]);
    p->crumb_count = 0;
}

/*
 * Append the first len bytes of path as a new breadcrumb entry, growing the
 * crumb_paths array if needed.
 *
 * p     The file picker whose breadcrumb array is updated.
 * path  Source path string.
 * len   Number of bytes to copy from path (result is null-terminated).
 * Returns true on success, false on allocation failure.
 */
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

/* Free all entry name and full_path strings and reset the entry count. */
static void fp_entries_clear(SolFilePicker *p)
{
    for (size_t i = 0; i < p->entry_count; ++i) {
        free(p->entries[i].name);
        free(p->entries[i].full_path);
    }
    p->entry_count = 0;
}

/*
 * Append a directory entry to the picker's entry array, growing it if needed.
 * Takes ownership of the heap-allocated name and full strings on success.
 *
 * p       The file picker to append to.
 * name    Heap-allocated basename string.
 * full    Heap-allocated absolute path string.
 * is_dir  Whether the entry is a directory.
 * Returns true on success, false on allocation failure (does not free strings).
 */
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

/*
 * Scan p->current_dir, populate the entries array with stat metadata, and
 * sort the results according to the picker's current sort column and direction.
 * Hidden/dotfile entries are excluded unless show_hidden is set.
 *
 * p  The file picker to refresh.
 */
static void fp_load_directory(SolFilePicker *p)
{
    fp_entries_clear(p);
    if (!p->current_dir) return;

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, p->current_dir)) return;

    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        if (entry.name[0] == '.') {
            /* Always skip the pseudo-entries "." and "..". */
            if (entry.name[1] == '\0') continue;
            if (entry.name[1] == '.' && entry.name[2] == '\0') continue;
            /* Skip other hidden/dotfile entries unless show_hidden is on. */
            if (!p->show_hidden) continue;
        }
        char *full = fp_path_join(p->current_dir, entry.name);
        if (!full) continue;
        char *name = fp_strdup(entry.name);
        if (!name) { free(full); continue; }
        if (!fp_entries_push(p, name, full, entry.is_directory)) {
            free(name); free(full); break;
        }
        /* Populate size / mtime from the filesystem. */
        {
            SolFpEntry *fe = &p->entries[p->entry_count - 1u];
            struct stat st;
            if (stat(fe->full_path, &st) == 0) {
                fe->size_bytes = entry.is_directory ? 0u : (uint64_t)st.st_size;
                fe->mtime      = (int64_t)st.st_mtime;
            }
        }
    }
    sol_platform_dir_close(&iter);
    g_sort_ctx = p;
    qsort(p->entries, p->entry_count, sizeof(SolFpEntry), fp_entry_cmp);
    g_sort_ctx = NULL;
}

/*
 * Change the picker's current directory, reload the directory listing, and
 * rebuild the breadcrumb segments.
 *
 * p     The file picker to navigate.
 * path  The target directory path (defaults to "." when empty or NULL).
 * Returns true on success, false on allocation failure.
 */
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

/*
 * Obtain the next available click context from the pool, growing it with
 * realloc when necessary.  Pointers into the pool remain valid for the
 * lifetime of the picker.
 *
 * p       The file picker owning the pool.
 * Returns Pointer to the next free context, or NULL on allocation failure.
 */
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

/*
 * Invoke the picker callback exactly once, guarded by the fired flag.
 *
 * p     The file picker.
 * path  The selected path, or NULL on cancellation.
 */
static void fp_fire(SolFilePicker *p, const char *path)
{
    if (p->fired) return;
    p->fired = true;
    if (p->callback) p->callback(path, p->user_data);
}

/* Fire the picker callback with path and close the window. */
static void fp_confirm(SolFilePicker *p, const char *path)
{
    fp_fire(p, path);
    if (p->window) ca_window_close(p->window);
}

/* Fire the picker callback with NULL (cancellation) and close the window. */
static void fp_cancel(SolFilePicker *p)
{
    fp_fire(p, NULL);
    if (p->window) ca_window_close(p->window);
}

/* ---------------------------------------------------------------- */
/* Click handlers                                                    */
/* ---------------------------------------------------------------- */

/*
 * Handle a click on a file-list row.  Navigates into directories; confirms
 * file selection in file-picker mode; ignores file clicks in folder mode.
 */
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

/*
 * Handle a click on a breadcrumb segment button, navigating to the
 * corresponding ancestor directory.
 */
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

/* Navigate to the parent of the current directory. */
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

/* Confirm the current directory as the selected folder and close the picker. */
static void fp_on_select_folder(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (p) fp_confirm(p, p->current_dir);
}

/* Cancel the picker, firing the callback with NULL and closing the window. */
static void fp_on_cancel(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (p) fp_cancel(p);
}

/* ---------------------------------------------------------------- */
/* Toolbar action handlers                                           */
/* ---------------------------------------------------------------- */

/* Navigate to the user's home directory ($HOME / %USERPROFILE%). */
static void fp_on_home_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
    if (!home || !home[0]) return;
    fp_set_current_dir(p, home);
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/* Reload the current directory listing and invalidate the content host. */
static void fp_on_refresh_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    fp_load_directory(p);
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/* Toggle visibility of hidden/dotfile entries and reload the listing. */
static void fp_on_toggle_hidden_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    p->show_hidden = !p->show_hidden;
    fp_load_directory(p);
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/* ---------------------------------------------------------------- */
/* Inline new-folder creation                                        */
/* ---------------------------------------------------------------- */

/*
 * Handle text-input change events for the inline new-folder creation bar.
 * Commits the new folder on Enter (key 257) and dismisses the bar on
 * Escape (key 256).
 */
static void fp_on_new_folder_change(Ca_TextInput *input, void *user_data)
{
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    const char *text = ca_get_text(input);
    if (text) {
        strncpy(p->new_folder_buf, text, sizeof(p->new_folder_buf) - 1u);
        p->new_folder_buf[sizeof(p->new_folder_buf) - 1u] = '\0';
    }
    /* Enter (GLFW_KEY_ENTER = 257): commit creation. */
    if (ca_input_key_pressed(input, 257) && p->new_folder_buf[0]) {
        char *new_path = fp_path_join(p->current_dir, p->new_folder_buf);
        if (new_path) {
            sol_platform_mkdir_p(new_path);
            fp_set_current_dir(p, new_path);
            free(new_path);
        }
        p->show_new_folder_bar = false;
        p->new_folder_buf[0]   = '\0';
        if (p->content_host) ca_div_invalidate(p->content_host);
        return;
    }
    /* Escape (GLFW_KEY_ESCAPE = 256): dismiss without creating. */
    if (ca_input_key_pressed(input, 256)) {
        p->show_new_folder_bar = false;
        p->new_folder_buf[0]   = '\0';
        if (p->content_host) ca_div_invalidate(p->content_host);
    }
}

/* Toggle the inline new-folder creation bar and request focus on the input. */
static void fp_on_new_folder_toggle_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    p->show_new_folder_bar    = !p->show_new_folder_bar;
    p->new_folder_buf[0]      = '\0';
    p->new_folder_needs_focus = p->show_new_folder_bar;
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/*
 * Create the folder named in new_folder_buf under the current directory,
 * navigate into it, and close the creation bar.
 */
static void fp_on_create_folder_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p || !p->new_folder_buf[0]) return;
    char *new_path = fp_path_join(p->current_dir, p->new_folder_buf);
    if (new_path) {
        sol_platform_mkdir_p(new_path);
        fp_set_current_dir(p, new_path);
        free(new_path);
    }
    p->show_new_folder_bar = false;
    p->new_folder_buf[0]   = '\0';
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/* Dismiss the inline new-folder bar without creating anything. */
static void fp_on_cancel_new_folder_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;
    p->show_new_folder_bar = false;
    p->new_folder_buf[0]   = '\0';
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/*
 * Handle a click on a sortable column header.  Toggles sort direction when
 * the same column is clicked again; otherwise switches to the new column
 * with a sensible default direction (date defaults to descending).
 */
static void fp_on_sort_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolFpClickCtx *ctx = (SolFpClickCtx *)user_data;
    if (!ctx || !ctx->picker) return;
    SolFilePicker *p = ctx->picker;
    if (p->sort_by == ctx->index) {
        p->sort_asc = !p->sort_asc;
    } else {
        p->sort_by  = ctx->index;
        /* Date defaults to newest-first on first click. */
        p->sort_asc = (ctx->index != FP_SORT_DATE);
    }
    g_sort_ctx = p;
    qsort(p->entries, p->entry_count, sizeof(SolFpEntry), fp_entry_cmp);
    g_sort_ctx = NULL;
    if (p->content_host) ca_div_invalidate(p->content_host);
}

/* ---------------------------------------------------------------- */
/* Rendering                                                         */
/* ---------------------------------------------------------------- */

/*
 * Emit the Causality nodes for a single file-list row at the given index,
 * including the type icon, name, size, and modification-date columns.
 *
 * p      The file picker owning the entry array.
 * index  Zero-based index into p->entries.
 */
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

    /* Size column */
    {
        char size_buf[24];
        fp_format_size(e->size_bytes, e->is_dir, size_buf, sizeof(size_buf));
        ca_text(&(Ca_TextDesc){ .text = size_buf, .style = "fp-row-size" });
    }
    /* Date column */
    {
        char date_buf[24];
        fp_format_date(e->mtime, date_buf, sizeof(date_buf));
        ca_text(&(Ca_TextDesc){ .text = date_buf, .style = "fp-row-date" });
    }

    ca_btn_end();
}

/*
 * Emit the horizontal breadcrumb toolbar row, rendering one clickable button
 * per path segment separated by "/" text labels.
 *
 * p  The file picker whose crumb_paths are rendered.
 */
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

/*
 * Emit one sortable column-header cell, including sort direction arrow when
 * this column is the active sort column.
 *
 * p            The file picker providing sort state.
 * cell_style   CSS class for the outer wrapper div.
 * btn_style    Reserved (currently unused).
 * label        Display text for the column header.
 * sort_col     FP_SORT_* identifier for this column.
 * justify_end  When true, the sort arrow is placed before the label text.
 */
static void fp_render_colhdr_cell(
        SolFilePicker *p, const char *cell_style, const char *btn_style,
        const char *label, int sort_col, bool justify_end)
{
    (void)btn_style;
    bool active = (p->sort_by == sort_col);

    SolFpClickCtx *ctx = fp_acquire_ctx(p);
    if (!ctx) return;
    ctx->picker = p;
    ctx->kind   = FP_CTX_SORT;
    ctx->index  = sort_col;

    /* Wrapper div owns the column width so the button can be width:100%. */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = cell_style });

    ca_btn_begin(&(Ca_BtnDesc){
        .style      = active
                          ? (justify_end ? "fp-colhdr-btn-end fp-colhdr-btn-active"
                                         : "fp-colhdr-btn fp-colhdr-btn-active")
                          : (justify_end ? "fp-colhdr-btn-end" : "fp-colhdr-btn"),
        .direction  = CA_HORIZONTAL,
        .on_click   = fp_on_sort_click,
        .click_data = ctx,
    });
    if (active && justify_end) {
        ca_text(&(Ca_TextDesc){
            .text  = p->sort_asc ? CA_ICON_FA_SORT_ASC : CA_ICON_FA_SORT_DESC,
            .style = "fp-colhdr-sort-arrow",
        });
    }
    ca_text(&(Ca_TextDesc){
        .text  = label,
        .style = active ? "fp-colhdr-text fp-colhdr-text-active"
                        : "fp-colhdr-text",
    });
    if (active && !justify_end) {
        ca_text(&(Ca_TextDesc){
            .text  = p->sort_asc ? CA_ICON_FA_SORT_ASC : CA_ICON_FA_SORT_DESC,
            .style = "fp-colhdr-sort-arrow",
        });
    }
    ca_btn_end();

    ca_div_end();
}

/*
 * Emit the full column-header row with Name, Size, and Modified cells.
 *
 * p  The file picker providing sort state.
 */
static void fp_render_column_header(SolFilePicker *p)
{
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "fp-colhdr" });

    /* 21-px icon-area spacer (tree-icon: width:16 + margin-right:5 = 21px). */
    ca_div_begin(&(Ca_DivDesc){ .style = "fp-colhdr-icon-gap" });
    ca_div_end();

    fp_render_colhdr_cell(p, "fp-colhdr-name-cell",  NULL, "Name",     FP_SORT_NAME, false);
    fp_render_colhdr_cell(p, "fp-colhdr-size-cell",  NULL, "Size",     FP_SORT_SIZE, true);
    fp_render_colhdr_cell(p, "fp-colhdr-date-cell",  NULL, "Modified", FP_SORT_DATE, true);

    ca_div_end();
}

/*
 * Causality reactive builder for the picker's content host div.  Emits the
 * toolbar, new-folder bar (when visible), column header, scrollable file
 * list, and footer with action buttons.  Called on every ca_div_invalidate.
 *
 * div        The content host div being rebuilt (unused directly).
 * user_data  Pointer to the SolFilePicker.
 */
static void fp_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolFilePicker *p = (SolFilePicker *)user_data;
    if (!p) return;

    /* Reset click pool for this rebuild.  The pool only grows so all
       pointers handed to causality in prior builds stay valid.      */
    p->click_ctx_count = 0u;

    /* ── Toolbar: up + breadcrumb + right actions ── */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "fp-toolbar" });
    {
        /* Up / parent directory */
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-up-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_up_click,
            .click_data = p,
        });
        ca_text(&(Ca_TextDesc){ .text = FP_ICON_ARROW_UP, .style = "fp-up-icon" });
        ca_btn_end();

        fp_render_breadcrumb(p);

        /* ── Right-side toolbar actions ── */

        /* Home directory */
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-up-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_home_click,
            .click_data = p,
        });
        ca_text(&(Ca_TextDesc){ .text = FP_ICON_HOME, .style = "fp-up-icon" });
        ca_btn_end();

        /* Refresh listing */
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-up-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_refresh_click,
            .click_data = p,
        });
        ca_text(&(Ca_TextDesc){ .text = FP_ICON_REFRESH, .style = "fp-up-icon" });
        ca_btn_end();

        /* Toggle hidden / dotfiles */
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "fp-up-btn",
            .direction  = CA_HORIZONTAL,
            .on_click   = fp_on_toggle_hidden_click,
            .click_data = p,
        });
        ca_text(&(Ca_TextDesc){
            .text  = p->show_hidden ? FP_ICON_EYE_SLASH : FP_ICON_EYE,
            .style = p->show_hidden ? "fp-up-icon fp-up-icon-active"
                                    : "fp-up-icon",
        });
        ca_btn_end();

    }
    ca_div_end();

    /* ── Inline new-folder creation bar ── */
    if (p->show_new_folder_bar) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL,
                                    .style     = "fp-new-folder-bar" });
        {
            ca_text(&(Ca_TextDesc){ .text  = "New folder:",
                                    .style = "fp-new-folder-label" });
            p->new_folder_input = ca_input(&(Ca_InputDesc){
                .placeholder = "folder name",
                .style       = "fp-new-folder-input",
                .on_change   = fp_on_new_folder_change,
                .change_data = p,
            });
            ca_btn_begin(&(Ca_BtnDesc){
                .text       = FP_LABEL_CREATE_FOLDER,
                .style      = "fp-nf-create",
                .on_click   = fp_on_create_folder_click,
                .click_data = p,
            });
            ca_btn_end();
            ca_btn_begin(&(Ca_BtnDesc){
                .text       = FP_LABEL_CANCEL,
                .style      = "fp-nf-cancel",
                .on_click   = fp_on_cancel_new_folder_click,
                .click_data = p,
            });
            ca_btn_end();
        }
        ca_div_end();
        /* Focus input once when the bar first opens. */
        if (p->new_folder_needs_focus && p->new_folder_input) {
            ca_input_focus(p->new_folder_input);
            p->new_folder_needs_focus = false;
        }
    }

    /* ── Column header (sortable) ── */
    fp_render_column_header(p);

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
        /* Left: item count info. */
        {
            char   fp_count_buf[64];
            size_t fp_dirs  = 0;
            size_t fp_files = 0;
            for (size_t ci = 0; ci < p->entry_count; ++ci) {
                if (p->entries[ci].is_dir) ++fp_dirs; else ++fp_files;
            }
            if (fp_dirs && fp_files)
                snprintf(fp_count_buf, sizeof(fp_count_buf),
                         "%zu folder%s, %zu file%s",
                         fp_dirs,  fp_dirs  == 1 ? "" : "s",
                         fp_files, fp_files == 1 ? "" : "s");
            else if (fp_dirs)
                snprintf(fp_count_buf, sizeof(fp_count_buf),
                         "%zu folder%s", fp_dirs, fp_dirs == 1 ? "" : "s");
            else if (fp_files)
                snprintf(fp_count_buf, sizeof(fp_count_buf),
                         "%zu file%s", fp_files, fp_files == 1 ? "" : "s");
            else
                fp_count_buf[0] = '\0';
            if (fp_count_buf[0])
                ca_text(&(Ca_TextDesc){ .text  = fp_count_buf,
                                        .style = "fp-footer-count" });
        }

        /* New Folder button — reliable placement in footer. */
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = FP_LABEL_NEW_FOLDER,
            .style      = p->show_new_folder_bar
                              ? "fp-action-new-folder fp-action-new-folder-active"
                              : "fp-action-new-folder",
            .on_click   = fp_on_new_folder_toggle_click,
            .click_data = p,
        });
        ca_btn_end();

        /* Spacer: pushes action buttons to the right. */
        ca_div_begin(&(Ca_DivDesc){ .style = "fp-footer-spacer" });
        ca_div_end();

        if (p->mode == SOL_FILE_PICKER_FOLDER) {
            ca_btn_begin(&(Ca_BtnDesc){
                .text       = FP_LABEL_SELECT_FOLDER,
                .style      = "fp-action-primary",
                .on_click   = fp_on_select_folder,
                .click_data = p,
            });
            ca_btn_end();
        }

        ca_btn_begin(&(Ca_BtnDesc){
            .text       = FP_LABEL_CANCEL,
            .style      = "fp-action-cancel",
            .on_click   = fp_on_cancel,
            .click_data = p,
        });
        ca_btn_end();
    }
    ca_div_end();
}

/*
 * Construct the top-level Causality layout for the picker window: a vertical
 * root div containing a single reactive content-host div.
 *
 * p  The file picker whose window layout is initialised.
 */
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

/*
 * Close the picker window (if still open) and free all heap memory owned by
 * the picker, including entries, breadcrumbs, click contexts, and the struct
 * itself.
 *
 * p  The file picker to destroy (safe to call with NULL).
 */
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

/*
 * Create and open a new file/folder picker window.
 * Resolves the initial directory (falls back to cwd), allocates the picker
 * struct, creates the Causality window, and registers the picker in the
 * global linked list so sol_file_picker_tick can manage its lifetime.
 *
 * instance     The Causality instance to create the window on.
 * mode         SOL_FILE_PICKER_FILE or SOL_FILE_PICKER_FOLDER.
 * initial_dir  Starting directory path (NULL or empty → current working dir).
 * on_select    Callback invoked with the chosen path, or NULL on cancel.
 * user_data    Passed through to on_select unchanged.
 * Returns      The new picker, or NULL on failure.
 */
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
    p->sort_asc  = true;   /* default: ascending by name */

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

/*
 * Advance picker lifecycle: walk the global picker list and destroy any
 * picker whose window has been closed, firing the callback with NULL if it
 * was never confirmed.  Call once per application frame.
 */
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
