#include "git_plugin.h"

#include <causality.h>

#include <stdlib.h>
#include <string.h>

typedef struct GitViewState {
    char *text;
    size_t length;
} GitViewState;

/* Free all storage owned by a Git output buffer. */
static void git_view_destroy(void *state)
{
    GitViewState *view = (GitViewState *)state;
    if (!view) return;
    free(view->text);
    free(view);
}

/* Select a text style for one diff, log, or blame output line. */
static const char *git_view_line_style(const char *line, size_t length)
{
    if (length >= 2u && line[0] == '+' && line[1] != '+') return "scm-view-added";
    if (length >= 2u && line[0] == '-' && line[1] != '-') return "scm-view-removed";
    if (length >= 2u && line[0] == '@' && line[1] == '@') return "scm-view-hunk";
    if ((length >= 7u && memcmp(line, "commit ", 7u) == 0) ||
        (length >= 4u && memcmp(line, "diff", 4u) == 0)) return "scm-view-heading";
    if ((length >= 4u && memcmp(line, "--- ", 4u) == 0) ||
        (length >= 4u && memcmp(line, "+++ ", 4u) == 0) ||
        (length >= 6u && memcmp(line, "Author", 6u) == 0) ||
        (length >= 4u && memcmp(line, "Date", 4u) == 0)) return "scm-view-meta";
    return "scm-view-text";
}

/* Render captured Git output as a scrollable, colorized custom buffer. */
static void git_view_render(const SolBuffer *buffer,
                            const SolBufferRenderArgs *args,
                            void *state)
{
    (void)buffer;
    (void)args;
    GitViewState *view = (GitViewState *)state;
    if (!view) return;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "scm-view",
    });
    size_t offset = 0u;
    while (offset < view->length) {
        size_t end = offset;
        while (end < view->length && view->text[end] != '\n') ++end;
        size_t length = end - offset;
        if (length > 8191u) length = 8191u;
        char line[8192];
        if (length > 0u) memcpy(line, view->text + offset, length);
        line[length] = '\0';
        ca_text(&(Ca_TextDesc){
            .text = line,
            .style = git_view_line_style(line, length),
        });
        offset = end < view->length ? end + 1u : end;
    }
    if (view->length == 0u) {
        ca_text(&(Ca_TextDesc){ .text = "No output.", .style = "scm-view-meta" });
    }
    ca_div_end();
}

/* Open captured Git output in a colored, read-only custom buffer. */
SolBufferId git_view_open(SolPluginCtx *ctx,
                          const char *title,
                          const char *text,
                          size_t text_length)
{
    if (!ctx || !title || (!text && text_length > 0u)) return 0u;
    GitViewState *view = (GitViewState *)calloc(1u, sizeof(*view));
    if (!view) return 0u;
    view->text = (char *)malloc(text_length + 1u);
    if (!view->text) {
        free(view);
        return 0u;
    }
    if (text_length > 0u) memcpy(view->text, text, text_length);
    view->text[text_length] = '\0';
    view->length = text_length;

    SolBufferId id = sol_plugin_open_custom(
        ctx, title, view,
        (SolBufferOps){ .destroy = git_view_destroy, .render = git_view_render });
    if (id == 0u) {
        git_view_destroy(view);
        return 0u;
    }
    (void)sol_plugin_focus_buffer(ctx, id);
    return id;
}
