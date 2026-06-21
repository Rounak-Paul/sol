// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_ui_internal.h"

#include "sol_platform.h"
#include "sol_rope.h"
#include "sol_search.h"
#include "sol_syntax.h"
#include "sol_syntax_highlight.h"
#include "sol_text_buffer.h"
#include "sol_threading.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_DEFAULT_WIDTH   1040
#define SEARCH_DEFAULT_HEIGHT  680
#define SEARCH_RESULT_MAX      200u
#define SEARCH_PREVIEW_LINES   36u
#define SEARCH_PREVIEW_LINE_BYTES 1024u
#define SEARCH_PREVIEW_TOKEN_MAX 512u
#define SEARCH_PREVIEW_TOKEN_RING 2048u
#define SEARCH_STREAM_INTERVAL_NS 16000000ull

#define SEARCH_KEY_ESCAPE 256
#define SEARCH_KEY_ENTER  257
#define SEARCH_KEY_DOWN   264
#define SEARCH_KEY_UP     265

typedef enum SolSearchWindowMode {
    SOL_SEARCH_WINDOW_FILES,
    SOL_SEARCH_WINDOW_CONTENTS,
} SolSearchWindowMode;

typedef struct SolSearchWindow SolSearchWindow;

typedef struct SolSearchRowCtx {
    SolSearchWindow *window;
    size_t           index;
} SolSearchRowCtx;

struct SolSearchWindow {
    Ca_Window           *window;
    SolUISystem         *ui;
    SolSearchWindowMode  mode;
    SolSearchIndex      *index;
    Ca_Signal           *sig_results_rev;
    Ca_Div              *content_host;
    Ca_TextInput        *input;
    bool                 needs_focus;
    char                 query[256];
    SolSearchResult      results[SEARCH_RESULT_MAX];
    size_t               result_count;
    size_t               selected;
    bool                 search_pending;
    pthread_t            search_thread;
    bool                 search_thread_started;
    _Atomic bool         search_cancel;
    _Atomic bool         search_done;
    _Atomic size_t       search_processed;
    _Atomic size_t       search_total;
    char                 worker_query[256];
    SolSearchResult      worker_results[SEARCH_RESULT_MAX];
    size_t               worker_result_count;
    pthread_mutex_t      stream_mutex;
    bool                 stream_mutex_initialized;
    SolSearchResult      stream_results[SEARCH_RESULT_MAX];
    size_t               stream_result_count;
    _Atomic uint32_t     stream_revision;
    uint32_t             shown_stream_revision;
    uint64_t             worker_last_publish_ns;
    size_t               shown_progress;
    bool                 search_has_completed;
    char                 preview_path[4096];
    SolMappedFile        preview_mapped;
    bool                 preview_mapped_valid;
    SolRope             *preview_rope;
    SolSyntaxHighlighter *preview_highlighter;
    SolSearchRowCtx      row_ctxs[SEARCH_RESULT_MAX];
    char                 status[160];
    SolSearchWindow     *next;
};

static SolSearchWindow *g_search_windows = NULL;
static char g_preview_token_ring[SEARCH_PREVIEW_TOKEN_RING][SEARCH_PREVIEW_TOKEN_MAX];
static size_t g_preview_token_ring_cursor = 0u;

/*
 * Return a pointer to the next available slot in the preview token ring
 * buffer.  The ring is large enough that slots remain valid for the entire
 * Causality frame in which they are written.
 */
static char *search_preview_token_slot(void)
{
    return g_preview_token_ring[
        g_preview_token_ring_cursor++ & (SEARCH_PREVIEW_TOKEN_RING - 1u)];
}

/*
 * Release all resources held by the preview cache: syntax highlighter, rope,
 * and memory-mapped file.  Resets all associated pointers and flags.
 *
 * w  The search window whose preview cache is cleared.
 */
static void search_clear_preview_cache(SolSearchWindow *w)
{
    if (!w) return;
    sol_syntax_highlight_destroy(w->preview_highlighter);
    sol_rope_destroy(w->preview_rope);
    if (w->preview_mapped_valid) sol_platform_unmap_file(&w->preview_mapped);
    w->preview_highlighter = NULL;
    w->preview_rope = NULL;
    w->preview_mapped_valid = false;
    w->preview_path[0] = '\0';
}

/*
 * Ensure the preview cache is loaded for path, returning immediately if the
 * file is already mapped.  On a cache miss the old file is released and the
 * new one is memory-mapped; a syntax highlighter and rope are created when a
 * language can be resolved for the path.
 *
 * w     The search window whose preview cache is updated.
 * path  Absolute filesystem path of the file to preview.
 * Returns true on success, false if the file could not be mapped.
 */
static bool search_prepare_preview(SolSearchWindow *w, const char *path)
{
    if (!w || !path) return false;
    if (w->preview_mapped_valid && strcmp(w->preview_path, path) == 0) {
        return true;
    }
    search_clear_preview_cache(w);
    if (!sol_platform_map_file_readonly(path, &w->preview_mapped, NULL)) {
        return false;
    }
    w->preview_mapped_valid = true;
    snprintf(w->preview_path, sizeof(w->preview_path), "%s", path);

    SolSyntaxRegistry *registry = sol_syntax_get_global_registry();
    const void *language = registry ? sol_syntax_get_for_path(registry, path) : NULL;
    if (language) {
        w->preview_rope = sol_rope_from_file(path, NULL);
        w->preview_highlighter = sol_syntax_highlight_create(
            language, sol_syntax_get_query_for_path(registry, path));
        if (w->preview_rope && w->preview_highlighter) {
            sol_syntax_highlight_reparse(w->preview_highlighter, w->preview_rope);
        }
    }
    return true;
}

/*
 * Emit Causality text nodes for one line of the file preview, applying syntax
 * highlight spans when a valid highlighter is available.  Uses token-ring
 * slots so the emitted strings survive until the frame is committed.
 *
 * line             Pointer to the first byte of the line content.
 * line_bytes       Number of bytes in the line (excluding the newline).
 * line_start_byte  Byte offset of the line's start within the file.
 * highlighter      The syntax highlighter to query for spans (may be NULL).
 */
static void search_render_preview_text(const char *line,
                                       size_t line_bytes,
                                       uint32_t line_start_byte,
                                       const SolSyntaxHighlighter *highlighter)
{
    if (!highlighter || !sol_syntax_highlight_is_valid(highlighter)) {
        char *slot = search_preview_token_slot();
        size_t length = line_bytes;
        if (length >= SEARCH_PREVIEW_TOKEN_MAX) {
            length = SEARCH_PREVIEW_TOKEN_MAX - 1u;
        }
        memcpy(slot, line, length);
        slot[length] = '\0';
        ca_text(&(Ca_TextDesc){
            .text = length > 0u ? slot : " ",
            .style = "search-preview-text",
        });
        return;
    }

    SolSyntaxSpan spans[64];
    const size_t span_count = sol_syntax_highlight_spans_for_range(
        highlighter, line_start_byte,
        line_start_byte + (uint32_t)line_bytes, spans, 64u);
    if (line_bytes == 0u || span_count == 0u) {
        char *slot = search_preview_token_slot();
        size_t length = line_bytes;
        if (length >= SEARCH_PREVIEW_TOKEN_MAX) {
            length = SEARCH_PREVIEW_TOKEN_MAX - 1u;
        }
        memcpy(slot, line, length);
        slot[length] = '\0';
        ca_text(&(Ca_TextDesc){
            .text = length > 0u ? slot : " ",
            .style = "hl-plain",
        });
        return;
    }

    const uint32_t line_end_byte = line_start_byte + (uint32_t)line_bytes;
    uint32_t position = line_start_byte;
    for (size_t i = 0u; i < span_count; ++i) {
        uint32_t start = spans[i].start_byte;
        uint32_t end = spans[i].end_byte;
        if (start < line_start_byte) start = line_start_byte;
        if (end > line_end_byte) end = line_end_byte;
        if (start >= end || start < position) continue;

        if (position < start) {
            size_t length = start - position;
            if (length >= SEARCH_PREVIEW_TOKEN_MAX) {
                length = SEARCH_PREVIEW_TOKEN_MAX - 1u;
            }
            char *slot = search_preview_token_slot();
            memcpy(slot, line + position - line_start_byte, length);
            slot[length] = '\0';
            ca_text(&(Ca_TextDesc){ .text = slot, .style = "hl-plain" });
        }

        size_t length = end - start;
        if (length >= SEARCH_PREVIEW_TOKEN_MAX) {
            length = SEARCH_PREVIEW_TOKEN_MAX - 1u;
        }
        char *slot = search_preview_token_slot();
        memcpy(slot, line + start - line_start_byte, length);
        slot[length] = '\0';
        ca_text(&(Ca_TextDesc){ .text = slot, .style = spans[i].css_class });
        position = end;
    }

    if (position < line_end_byte) {
        size_t length = line_end_byte - position;
        if (length >= SEARCH_PREVIEW_TOKEN_MAX) {
            length = SEARCH_PREVIEW_TOKEN_MAX - 1u;
        }
        char *slot = search_preview_token_slot();
        memcpy(slot, line + position - line_start_byte, length);
        slot[length] = '\0';
        ca_text(&(Ca_TextDesc){ .text = slot, .style = "hl-plain" });
    }
}

/*
 * Progress callback invoked from the search worker thread.  Updates atomic
 * progress counters, publishes an intermediate result snapshot to the UI
 * thread at most once per SEARCH_STREAM_INTERVAL_NS, and checks for
 * cancellation.
 *
 * results       Current result array (may be NULL at some progress points).
 * result_count  Number of valid entries in results.
 * processed     Number of files examined so far.
 * total         Total number of files to examine.
 * user_data     Pointer to the SolSearchWindow.
 * Returns       true to continue searching, false to abort.
 */
static bool search_worker_progress(const SolSearchResult *results,
                                   size_t result_count,
                                   size_t processed,
                                   size_t total,
                                   void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    atomic_store_explicit(&w->search_processed, processed, memory_order_relaxed);
    atomic_store_explicit(&w->search_total, total, memory_order_relaxed);
    if (atomic_load_explicit(&w->search_cancel, memory_order_relaxed)) {
        return false;
    }

    const uint64_t now = sol_platform_now_monotonic_ns();
    if (results && (processed == total ||
                    now - w->worker_last_publish_ns >= SEARCH_STREAM_INTERVAL_NS)) {
        pthread_mutex_lock(&w->stream_mutex);
        w->stream_result_count = result_count;
        memcpy(w->stream_results, results,
               result_count * sizeof(SolSearchResult));
        pthread_mutex_unlock(&w->stream_mutex);
        w->worker_last_publish_ns = now;
        atomic_fetch_add_explicit(
            &w->stream_revision, 1u, memory_order_release);
        ca_instance_wake();
    }
    return true;
}

/*
 * Thread entry point for background content searches.  Runs
 * sol_search_contents_progress to completion, then sets search_done and
 * wakes the Causality instance.
 *
 * user_data  Pointer to the SolSearchWindow.
 * Returns    NULL.
 */
static void *search_worker_main(void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    w->worker_result_count = sol_search_contents_progress(
        w->index, w->worker_query, w->worker_results, SEARCH_RESULT_MAX,
        search_worker_progress, w);
    atomic_store_explicit(&w->search_done, true, memory_order_release);
    ca_instance_wake();
    return NULL;
}

/* Signal the search worker to stop at the next cancellation check point. */
static void search_cancel_worker(SolSearchWindow *w)
{
    if (!w || !w->search_thread_started) return;
    atomic_store_explicit(&w->search_cancel, true, memory_order_relaxed);
}

/* Wait for the search worker thread to finish, then mark it as stopped. */
static void search_join_worker(SolSearchWindow *w)
{
    if (!w || !w->search_thread_started) return;
    pthread_join(w->search_thread, NULL);
    w->search_thread_started = false;
}

/*
 * Start a new background content-search worker thread for w->query.
 * Resets all progress and stream-revision state before spawning.
 *
 * w       The search window to operate on.
 * Returns true when the thread was successfully started.
 */
static bool search_start_worker(SolSearchWindow *w)
{
    if (!w || w->search_thread_started || !w->query[0]) return false;
    snprintf(w->worker_query, sizeof(w->worker_query), "%s", w->query);
    w->worker_result_count = 0u;
    w->stream_result_count = 0u;
    w->shown_stream_revision = 0u;
    w->worker_last_publish_ns = 0u;
    atomic_store_explicit(&w->stream_revision, 0u, memory_order_relaxed);
    atomic_store_explicit(&w->search_cancel, false, memory_order_relaxed);
    atomic_store_explicit(&w->search_done, false, memory_order_relaxed);
    atomic_store_explicit(&w->search_processed, 0u, memory_order_relaxed);
    atomic_store_explicit(
        &w->search_total, sol_search_index_file_count(w->index), memory_order_relaxed);
    w->shown_progress = 0u;
    w->search_has_completed = false;
    if (pthread_create(&w->search_thread, NULL, search_worker_main, w) != 0) {
        return false;
    }
    w->search_thread_started = true;
    w->search_pending = false;
    snprintf(w->status, sizeof(w->status), "Searching...");
    return true;
}

/*
 * Execute a synchronous fuzzy file-name search and update w->results,
 * w->result_count, w->selected, and the status string.
 *
 * w  The search window to refresh (must be in FILES mode).
 */
static void search_refresh(SolSearchWindow *w)
{
    if (!w || !w->index) return;
    w->result_count = sol_search_files(
        w->index, w->query, w->results, SEARCH_RESULT_MAX);
    if (w->selected >= w->result_count) {
        w->selected = w->result_count ? w->result_count - 1u : 0u;
    }
    snprintf(w->status, sizeof(w->status), "%zu results in %zu files",
             w->result_count, sol_search_index_file_count(w->index));
}

/*
 * Open the result at result_index via the file-open callback, position the
 * cursor at the matched line when available, and close the search window.
 *
 * w             The search window initiating the open.
 * result_index  Index into w->results of the result to open.
 */
static void search_open_result(SolSearchWindow *w, size_t result_index)
{
    if (!w || !w->ui || result_index >= w->result_count) return;
    const SolSearchResult *result = &w->results[result_index];
    if (!w->ui->file_open_callback ||
        !w->ui->file_open_callback(result->full_path, w->ui->file_open_user_data)) {
        return;
    }

    if (result->line_number > 0u) {
        SolTextBuffer *buffer = sol_text_buffer_active(w->ui->buffers);
        if (buffer) {
            sol_text_buffer_set_cursor_to(buffer, result->line_number - 1u, 0u);
            const int scroll_top = result->line_number > 4u
                ? (int)result->line_number - 4
                : 0;
            sol_text_buffer_set_scroll_top(buffer, scroll_top);
        }
    }
    sol_ui_system_invalidate_buffer_area(w->ui);
    if (w->window && ca_window_is_open(w->window)) ca_window_close(w->window);
}

/*
 * Handle a click on a result row: update the selected index and bump
 * sig_results_rev to refresh the preview and highlight.
 */
static void search_on_row_click(Ca_Button *button, void *user_data)
{
    (void)button;
    SolSearchRowCtx *ctx = (SolSearchRowCtx *)user_data;
    if (!ctx || !ctx->window || ctx->index >= ctx->window->result_count) return;
    ctx->window->selected = ctx->index;
    sol_ui_bump_u32(ctx->window->sig_results_rev);
}

/*
 * Handle query input changes.  For file-name mode, re-runs the synchronous
 * search immediately; for contents mode, cancels any running worker and
 * schedules a new search on the next frame tick.
 */
static void search_on_input_change(Ca_TextInput *input, void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    if (!w) return;
    const char *text = ca_get_text(input);
    snprintf(w->query, sizeof(w->query), "%s", text ? text : "");
    w->selected = 0u;
    if (w->mode == SOL_SEARCH_WINDOW_CONTENTS) {
        search_cancel_worker(w);
        w->search_pending = true;
        w->search_has_completed = false;
        w->result_count = 0u;
        snprintf(w->status, sizeof(w->status),
                 w->query[0] ? "Starting search..." : "Type to search");
    } else {
        search_refresh(w);
    }
    sol_ui_bump_u32(w->sig_results_rev);
}

/*
 * Emit the Causality button for one search result row, showing the relative
 * path and optional line number.
 *
 * w      The search window providing result data and selection state.
 * index  Zero-based index into w->results.
 */
static void search_render_result(SolSearchWindow *w, size_t index)
{
    const SolSearchResult *result = &w->results[index];
    SolSearchRowCtx *ctx = &w->row_ctxs[index];
    ctx->window = w;
    ctx->index = index;

    ca_btn_begin(&(Ca_BtnDesc){
        .direction  = CA_HORIZONTAL,
        .style      = index == w->selected
            ? "search-result search-result-selected"
            : "search-result",
        .on_click   = search_on_row_click,
        .click_data = ctx,
    });

    ca_text(&(Ca_TextDesc){
        .text  = result->relative_path,
        .style = "search-result-path",
    });
    if (result->line_number > 0u) {
        char line[32];
        snprintf(line, sizeof(line), "line %zu", result->line_number);
        ca_text(&(Ca_TextDesc){ .text = line, .style = "search-result-line" });
    }
    ca_btn_end();
}

/*
 * Emit the right-side preview panel for the currently selected result.
 * Reads up to SEARCH_PREVIEW_LINES lines from the memory-mapped file,
 * applying syntax highlighting when available, and highlights the matched
 * line.
 *
 * w  The search window providing result and preview-cache data.
 */
static void search_render_preview(SolSearchWindow *w)
{
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "search-preview",
    });

    if (w->result_count == 0u || w->selected >= w->result_count) {
        ca_text(&(Ca_TextDesc){
            .text = "Select a result to preview",
            .style = "search-preview-empty",
        });
        ca_div_end();
        return;
    }

    const SolSearchResult *result = &w->results[w->selected];
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "search-preview-header",
    });
    ca_text(&(Ca_TextDesc){
        .text = result->relative_path,
        .style = "search-preview-title",
    });
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "search-preview-code",
    });

    if (!search_prepare_preview(w, result->full_path)) {
        ca_text(&(Ca_TextDesc){
            .text = "Preview unavailable",
            .style = "search-preview-empty",
        });
        ca_div_end();
        ca_div_end();
        return;
    }
    const SolMappedFile *mapped = &w->preview_mapped;

    const size_t target = result->line_number > 0u ? result->line_number : 1u;
    const size_t first = target > 8u ? target - 8u : 1u;
    const size_t last = first + SEARCH_PREVIEW_LINES - 1u;
    size_t line_number = 1u;
    size_t line_start = 0u;
    while (line_start < mapped->size_bytes && line_number <= last) {
        size_t line_end = line_start;
        while (line_end < mapped->size_bytes && mapped->data[line_end] != '\n') {
            line_end++;
        }
        if (line_number >= first) {
            char number[32];
            char text[SEARCH_PREVIEW_LINE_BYTES];
            size_t length = line_end - line_start;
            if (length > 0u && mapped->data[line_start + length - 1u] == '\r') {
                length--;
            }
            if (length >= sizeof(text)) length = sizeof(text) - 1u;
            memcpy(text, mapped->data + line_start, length);
            text[length] = '\0';
            snprintf(number, sizeof(number), "%zu", line_number);
            char *number_slot = search_preview_token_slot();
            snprintf(number_slot, SEARCH_PREVIEW_TOKEN_MAX, "%s", number);

            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_HORIZONTAL,
                .style = line_number == target
                    ? "search-preview-row search-preview-row-match"
                    : "search-preview-row",
            });
            ca_text(&(Ca_TextDesc){
                .text = number_slot,
                .style = "search-preview-number",
            });
            search_render_preview_text(
                text, length, (uint32_t)line_start, w->preview_highlighter);
            ca_div_end();
        }
        line_start = line_end + 1u;
        line_number++;
    }
    ca_div_end();
    ca_div_end();
}

/*
 * Reactive builder for the search window's body div.  Subscribes to
 * sig_results_rev and emits the title, query input, horizontal split
 * (results list | preview), and footer with progress bar and status.
 *
 * div        The body div being rebuilt (unused directly).
 * user_data  Pointer to the SolSearchWindow.
 */
static void search_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    (void)ca_signal_get_u32(w->sig_results_rev);

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "search-header",
    });
    ca_text(&(Ca_TextDesc){
        .text  = w->mode == SOL_SEARCH_WINDOW_FILES
            ? "FIND FILES" : "SEARCH WORKSPACE CONTENTS",
        .style = "search-title",
    });
    w->input = ca_input(&(Ca_InputDesc){
        .text        = w->query,
        .placeholder = w->mode == SOL_SEARCH_WINDOW_FILES
            ? "Type a fuzzy file path..."
            : "Type text to find across files...",
        .on_change   = search_on_input_change,
        .change_data = w,
        .style       = "search-input",
    });
    ca_text(&(Ca_TextDesc){
        .text  = sol_search_index_root(w->index),
        .style = "search-root",
    });
    ca_div_end();

    ca_split_begin(&(Ca_SplitDesc){
        .direction = CA_HORIZONTAL,
        .ratio = 0.36f,
        .min_ratio = 0.24f,
        .max_ratio = 0.62f,
        .bar_size = 1.0f,
        .style = "search-split",
    });
    {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "search-results",
            .id        = "search-results-list",
        });
        if (w->result_count == 0u) {
            ca_text(&(Ca_TextDesc){
                .text  = w->search_thread_started || w->search_pending
                    ? "Searching..."
                    : (w->mode == SOL_SEARCH_WINDOW_CONTENTS && !w->query[0]
                        ? "Start typing to search file contents"
                        : "No matches"),
                .style = "search-empty",
            });
        } else {
            for (size_t i = 0u; i < w->result_count; ++i) {
                search_render_result(w, i);
            }
        }
        ca_div_end();
        search_render_preview(w);
    }
    ca_split_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "search-footer",
    });
    const size_t processed =
        atomic_load_explicit(&w->search_processed, memory_order_relaxed);
    const size_t total =
        atomic_load_explicit(&w->search_total, memory_order_relaxed);
    const bool show_progress =
        w->mode == SOL_SEARCH_WINDOW_CONTENTS && w->query[0];
    float progress_value = 0.0f;
    if (w->search_thread_started && total > 0u) {
        progress_value = (float)processed / (float)total;
    } else if (w->search_pending) {
        progress_value = 0.02f;
    } else if (w->search_has_completed) {
        progress_value = 1.0f;
    }
    ca_progress(&(Ca_ProgressDesc){
        .value = progress_value,
        .width = 180.0f,
        .height = 6.0f,
        .style = "search-progress",
        .hidden = !show_progress,
    });
    ca_text(&(Ca_TextDesc){ .text = w->status, .style = "search-status" });
    ca_text(&(Ca_TextDesc){
        .text  = "Up/Down select   Enter open   Esc close",
        .style = "search-help",
    });
    ca_div_end();
}

/*
 * Per-frame callback for the search window.  Drives the background search
 * lifecycle (focus, worker completion, streaming results, progress updates,
 * pending restart) and handles keyboard shortcuts (Escape, Enter, Up, Down).
 *
 * user_data  Pointer to the SolSearchWindow.
 */
static void search_on_frame(void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    if (!w || !w->input) return;
    if (w->needs_focus) {
        ca_input_focus(w->input);
        w->needs_focus = false;
    }

    if (w->search_thread_started &&
        atomic_load_explicit(&w->search_done, memory_order_acquire)) {
        const bool cancelled =
            atomic_load_explicit(&w->search_cancel, memory_order_relaxed);
        search_join_worker(w);
        if (!cancelled && strcmp(w->worker_query, w->query) == 0) {
            w->search_has_completed = true;
            w->result_count = w->worker_result_count;
            memcpy(w->results, w->worker_results,
                   w->result_count * sizeof(SolSearchResult));
            if (w->selected >= w->result_count) {
                w->selected = w->result_count ? w->result_count - 1u : 0u;
            }
            snprintf(w->status, sizeof(w->status), "%zu results in %zu files",
                     w->result_count, sol_search_index_file_count(w->index));
        }
        sol_ui_bump_u32(w->sig_results_rev);
    }

    const uint32_t stream_revision =
        atomic_load_explicit(&w->stream_revision, memory_order_acquire);
    if (w->search_thread_started &&
        stream_revision != w->shown_stream_revision &&
        strcmp(w->worker_query, w->query) == 0) {
        pthread_mutex_lock(&w->stream_mutex);
        w->result_count = w->stream_result_count;
        memcpy(w->results, w->stream_results,
               w->result_count * sizeof(SolSearchResult));
        pthread_mutex_unlock(&w->stream_mutex);
        w->shown_stream_revision = stream_revision;
        if (w->selected >= w->result_count) {
            w->selected = w->result_count ? w->result_count - 1u : 0u;
        }
        sol_ui_bump_u32(w->sig_results_rev);
    }

    if (w->search_pending && !w->search_thread_started) {
        if (!w->query[0]) {
            w->search_pending = false;
            w->result_count = 0u;
            snprintf(w->status, sizeof(w->status), "Type to search");
        } else if (!search_start_worker(w)) {
            w->search_pending = false;
            snprintf(w->status, sizeof(w->status), "Search could not start");
        }
        sol_ui_bump_u32(w->sig_results_rev);
    }

    if (w->search_thread_started) {
        const size_t processed =
            atomic_load_explicit(&w->search_processed, memory_order_relaxed);
        const size_t total =
            atomic_load_explicit(&w->search_total, memory_order_relaxed);
        if (processed != w->shown_progress) {
            w->shown_progress = processed;
            snprintf(w->status, sizeof(w->status), "Searching %zu / %zu files",
                     processed, total);
            sol_ui_bump_u32(w->sig_results_rev);
        }
    }

    if (ca_input_key_pressed(w->input, SEARCH_KEY_ESCAPE)) {
        ca_window_close(w->window);
        return;
    }
    if (ca_input_key_pressed(w->input, SEARCH_KEY_ENTER)) {
        search_open_result(w, w->selected);
        return;
    }

    size_t previous = w->selected;
    if (ca_input_key_pressed(w->input, SEARCH_KEY_DOWN) && w->result_count > 0u) {
        w->selected = (w->selected + 1u) % w->result_count;
    }
    if (ca_input_key_pressed(w->input, SEARCH_KEY_UP) && w->result_count > 0u) {
        w->selected = (w->selected + w->result_count - 1u) % w->result_count;
    }
    if (w->selected != previous) {
        sol_ui_bump_u32(w->sig_results_rev);
        ca_set_scroll_y(w->window, "search-results-list",
                        (float)w->selected * 30.0f);
    }
}

/*
 * Cancel any running worker, close the window, and free all resources owned
 * by the search window including the search index and preview cache.
 *
 * w  The search window to destroy (safe to call with NULL).
 */
static void search_destroy(SolSearchWindow *w)
{
    if (!w) return;
    search_cancel_worker(w);
    search_join_worker(w);
    if (w->window && ca_window_is_open(w->window)) ca_window_close(w->window);
    search_clear_preview_cache(w);
    sol_search_index_destroy(w->index);
    if (w->stream_mutex_initialized) pthread_mutex_destroy(&w->stream_mutex);
    free(w);
}

/*
 * Internal implementation for opening a search window.  Creates the window,
 * search index, reactive content div, and per-frame callback.  No-ops when a
 * search window is already open.
 *
 * ui    The UI system providing the Causality instance and file-open callback.
 * mode  SOL_SEARCH_WINDOW_FILES for fuzzy file search, or
 *       SOL_SEARCH_WINDOW_CONTENTS for grep-style content search.
 */
static void search_open(SolUISystem *ui, SolSearchWindowMode mode)
{
    if (!ui || !ui->instance) return;
    for (SolSearchWindow *existing = g_search_windows; existing; existing = existing->next) {
        if (existing->window && ca_window_is_open(existing->window)) {
            return;
        }
    }

    char cwd[4096];
    const char *root = sol_ui_system_file_tree_root(ui);
    if (!root || !root[0]) {
        root = sol_platform_get_cwd(cwd, sizeof(cwd)) ? cwd : ".";
    }

    SolSearchWindow *w = (SolSearchWindow *)calloc(1u, sizeof(SolSearchWindow));
    if (!w) return;
    w->ui = ui;
    w->mode = mode;
    atomic_init(&w->search_cancel, false);
    atomic_init(&w->search_done, false);
    atomic_init(&w->search_processed, 0u);
    atomic_init(&w->search_total, 0u);
    atomic_init(&w->stream_revision, 0u);
    if (pthread_mutex_init(&w->stream_mutex, NULL) != 0) {
        free(w);
        return;
    }
    w->stream_mutex_initialized = true;
    w->index = sol_search_index_create(root);
    w->sig_results_rev = ca_signal_u32(ui->instance, 0u);
    if (!w->index || !w->sig_results_rev) {
        search_destroy(w);
        return;
    }

    if (mode == SOL_SEARCH_WINDOW_FILES) {
        search_refresh(w);
    } else {
        snprintf(w->status, sizeof(w->status), "Type to search");
    }
    w->window = ca_window_create(ui->instance, &(Ca_WindowDesc){
        .title  = mode == SOL_SEARCH_WINDOW_FILES
            ? "Find Files" : "Search Workspace Contents",
        .width  = SEARCH_DEFAULT_WIDTH,
        .height = SEARCH_DEFAULT_HEIGHT,
    });
    if (!w->window) {
        search_destroy(w);
        return;
    }

    ca_ui_begin(w->window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "search-root-window",
    });
    w->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "search-body",
    });
    ca_div_set_builder(w->content_host, search_content_builder, w);
    ca_div_end();
    ca_ui_end();

    w->needs_focus = true;
    ca_window_set_on_frame(w->window, search_on_frame, w);
    w->next = g_search_windows;
    g_search_windows = w;
}

/* Open the fuzzy file-name search window. */
void sol_ui_search_window_open_files(SolUISystem *ui)
{
    search_open(ui, SOL_SEARCH_WINDOW_FILES);
}

/* Open the workspace content (grep-style) search window. */
void sol_ui_search_window_open_contents(SolUISystem *ui)
{
    search_open(ui, SOL_SEARCH_WINDOW_CONTENTS);
}

/*
 * Advance search-window lifecycle: walk the global list and destroy any
 * window whose Causality window has been closed.  Call once per frame.
 */
void sol_ui_search_window_tick(void)
{
    SolSearchWindow **link = &g_search_windows;
    while (*link) {
        SolSearchWindow *w = *link;
        if (!w->window || !ca_window_is_open(w->window)) {
            *link = w->next;
            w->window = NULL;
            search_destroy(w);
            continue;
        }
        link = &w->next;
    }
}
