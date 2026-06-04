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
#define SEARCH_CONTENT_DEBOUNCE_NS 75000000ull
#define SEARCH_PREVIEW_LINES   36u
#define SEARCH_PREVIEW_LINE_BYTES 1024u
#define SEARCH_PREVIEW_TOKEN_MAX 512u
#define SEARCH_PREVIEW_TOKEN_RING 2048u

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
    uint64_t             query_changed_ns;
    pthread_t            search_thread;
    bool                 search_thread_started;
    _Atomic bool         search_cancel;
    _Atomic bool         search_done;
    _Atomic size_t       search_processed;
    _Atomic size_t       search_total;
    char                 worker_query[256];
    SolSearchResult      worker_results[SEARCH_RESULT_MAX];
    size_t               worker_result_count;
    size_t               shown_progress;
    bool                 search_has_completed;
    SolSearchRowCtx      row_ctxs[SEARCH_RESULT_MAX];
    char                 status[160];
    SolSearchWindow     *next;
};

static SolSearchWindow *g_search_windows = NULL;
static char g_preview_token_ring[SEARCH_PREVIEW_TOKEN_RING][SEARCH_PREVIEW_TOKEN_MAX];
static size_t g_preview_token_ring_cursor = 0u;

static char *search_preview_token_slot(void)
{
    return g_preview_token_ring[
        g_preview_token_ring_cursor++ & (SEARCH_PREVIEW_TOKEN_RING - 1u)];
}

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

static bool search_worker_progress(size_t processed, size_t total, void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    atomic_store_explicit(&w->search_processed, processed, memory_order_relaxed);
    atomic_store_explicit(&w->search_total, total, memory_order_relaxed);
    return !atomic_load_explicit(&w->search_cancel, memory_order_relaxed);
}

static void *search_worker_main(void *user_data)
{
    SolSearchWindow *w = (SolSearchWindow *)user_data;
    w->worker_result_count = sol_search_contents_progress(
        w->index, w->worker_query, w->worker_results, SEARCH_RESULT_MAX,
        search_worker_progress, w);
    atomic_store_explicit(&w->search_done, true, memory_order_release);
    return NULL;
}

static void search_cancel_worker(SolSearchWindow *w)
{
    if (!w || !w->search_thread_started) return;
    atomic_store_explicit(&w->search_cancel, true, memory_order_relaxed);
}

static void search_join_worker(SolSearchWindow *w)
{
    if (!w || !w->search_thread_started) return;
    pthread_join(w->search_thread, NULL);
    w->search_thread_started = false;
}

static bool search_start_worker(SolSearchWindow *w)
{
    if (!w || w->search_thread_started || !w->query[0]) return false;
    snprintf(w->worker_query, sizeof(w->worker_query), "%s", w->query);
    w->worker_result_count = 0u;
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

static void search_on_row_click(Ca_Button *button, void *user_data)
{
    (void)button;
    SolSearchRowCtx *ctx = (SolSearchRowCtx *)user_data;
    if (!ctx || !ctx->window || ctx->index >= ctx->window->result_count) return;
    ctx->window->selected = ctx->index;
    sol_ui_bump_u32(ctx->window->sig_results_rev);
}

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
        w->query_changed_ns = sol_platform_now_monotonic_ns();
        w->result_count = 0u;
        snprintf(w->status, sizeof(w->status),
                 w->query[0] ? "Starting search..." : "Type to search");
    } else {
        search_refresh(w);
    }
    sol_ui_bump_u32(w->sig_results_rev);
}

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

    SolMappedFile mapped;
    if (!sol_platform_map_file_readonly(result->full_path, &mapped, NULL)) {
        ca_text(&(Ca_TextDesc){
            .text = "Preview unavailable",
            .style = "search-preview-empty",
        });
        ca_div_end();
        ca_div_end();
        return;
    }

    SolRope *preview_rope = NULL;
    SolSyntaxHighlighter *highlighter = NULL;
    SolSyntaxRegistry *registry = sol_syntax_get_global_registry();
    const void *language = registry
        ? sol_syntax_get_for_path(registry, result->full_path)
        : NULL;
    if (language) {
        preview_rope = sol_rope_from_file(result->full_path, NULL);
        highlighter = sol_syntax_highlight_create(
            language, sol_syntax_get_query_for_path(registry, result->full_path));
        if (preview_rope && highlighter) {
            sol_syntax_highlight_reparse(highlighter, preview_rope);
        }
    }

    const size_t target = result->line_number > 0u ? result->line_number : 1u;
    const size_t first = target > 8u ? target - 8u : 1u;
    const size_t last = first + SEARCH_PREVIEW_LINES - 1u;
    size_t line_number = 1u;
    size_t line_start = 0u;
    while (line_start < mapped.size_bytes && line_number <= last) {
        size_t line_end = line_start;
        while (line_end < mapped.size_bytes && mapped.data[line_end] != '\n') {
            line_end++;
        }
        if (line_number >= first) {
            char number[32];
            char text[SEARCH_PREVIEW_LINE_BYTES];
            size_t length = line_end - line_start;
            if (length > 0u && mapped.data[line_start + length - 1u] == '\r') {
                length--;
            }
            if (length >= sizeof(text)) length = sizeof(text) - 1u;
            memcpy(text, mapped.data + line_start, length);
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
                text, length, (uint32_t)line_start, highlighter);
            ca_div_end();
        }
        line_start = line_end + 1u;
        line_number++;
    }
    sol_syntax_highlight_destroy(highlighter);
    sol_rope_destroy(preview_rope);
    sol_platform_unmap_file(&mapped);
    ca_div_end();
    ca_div_end();
}

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
        .bar_color = 0x343442ff,
        .bar_hover_color = 0x4c82b0ff,
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
        .bar_color = 0x62a6d8ff,
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

    if (w->search_pending && !w->search_thread_started &&
        sol_platform_now_monotonic_ns() - w->query_changed_ns >=
            SEARCH_CONTENT_DEBOUNCE_NS) {
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

static void search_destroy(SolSearchWindow *w)
{
    if (!w) return;
    search_cancel_worker(w);
    search_join_worker(w);
    if (w->window && ca_window_is_open(w->window)) ca_window_close(w->window);
    sol_search_index_destroy(w->index);
    free(w);
}

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

void sol_ui_search_window_open_files(SolUISystem *ui)
{
    search_open(ui, SOL_SEARCH_WINDOW_FILES);
}

void sol_ui_search_window_open_contents(SolUISystem *ui)
{
    search_open(ui, SOL_SEARCH_WINDOW_CONTENTS);
}

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
