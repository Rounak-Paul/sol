#include <causality.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sol_buffer.h"
#include "sol_ui_constants.h"
#include "sol_system_manager.h"
#include "sol_ui_system.h"
#include "sol_file_picker.h"

typedef struct SolStartupPayload {
    uint32_t worker_count;
    uint32_t loaded_plugins;
    uint64_t warmup_checksum;
    bool input_binding_active;
} SolStartupPayload;

typedef struct SolWarmupContext {
    _Atomic uint64_t checksum;
} SolWarmupContext;

typedef struct SolAppContext {
    SolSystemManager *systems;
    SolEventBus *events;
    SolBufferSystem *buffers;
    SolJobSystem *jobs;
    SolInputSystem *input;
    SolSubscriptionToken startup_token;
    bool command_flows_ready;
    SolUISystem *ui;
    Ca_Instance *instance;
    /* Last-known cursor position in window CSS pixels; updated on every
       MOUSE_MOVE so MOUSE_SCROLL can route the wheel to the pane under
       the cursor (instead of the keyboard-focused one). */
    double mouse_x;
    double mouse_y;
} SolAppContext;

typedef struct SolTextBufferState {
    char  *text;        /* owns the file bytes; newlines replaced with NUL */
    char **lines;       /* line_count pointers into `text` */
    size_t line_count;
    int    scroll_top;  /* index of the first visible line */
    char  *source_path; /* absolute path on disk; NULL for unsaved/scratch */
} SolTextBufferState;

static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, value, len + 1u);
    return copy;
}

/* Slurp a whole file into a fresh, NUL-terminated heap buffer.
 * Returns NULL on any I/O error; *out_len is set on success. */
static char *sol_read_file_to_string(const char *path, size_t *out_len)
{
    if (!path) {
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long end = ftell(fp);
    if (end < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    size_t size = (size_t)end;
    char *buf = (char *)malloc(size + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t got = fread(buf, 1u, size, fp);
    fclose(fp);
    if (got != size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    if (out_len) {
        *out_len = size;
    }
    return buf;
}

static const char *sol_basename(const char *path)
{
    if (!path || !*path) {
        return "untitled";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sol_text_buffer_destroy(void *state)
{
    SolTextBufferState *text_state = (SolTextBufferState *)state;
    if (!text_state) {
        return;
    }

    free(text_state->source_path);
    free(text_state->lines);
    free(text_state->text);
    free(text_state);
}

/* Fixed line geometry — must match `.buffer-line` height in style.h.
   Used to size the visible window and the custom scrollbar. */
#define SOL_TEXT_LINE_HEIGHT_PX 20

/* Estimate of UI chrome above/below the buffer pane in CSS pixels.
   Title bar (~30) + status bar (22) + tabs row (28) + buffer-text-col
   vertical padding (16) + a small fudge for the inter-strip borders.
   The result is only used to derive the visible-line count, so it
   doesn't have to be exact, but if it is too SMALL we'll under-render
   and leave empty space at the bottom of the pane. */
#define SOL_TEXT_PANE_CHROME_PX 100

static int sol_text_visible_lines(int window_h)
{
    int avail = window_h - SOL_TEXT_PANE_CHROME_PX;
    if (avail < SOL_TEXT_LINE_HEIGHT_PX) avail = SOL_TEXT_LINE_HEIGHT_PX;
    int n = avail / SOL_TEXT_LINE_HEIGHT_PX;
    /* Over-render by a couple of rows so the column always fully fills
       the parent pane even when our chrome estimate is slightly off.
       The parent has overflow:hidden so extra rows just clip. */
    n += 2;
    if (n < 1) n = 1;
    return n;
}

static void sol_text_buffer_render(const SolBuffer *buffer, const SolBufferRenderArgs *args, void *state)
{
    (void)buffer;

    SolTextBufferState *ts = (SolTextBufferState *)state;
    if (!ts) {
        return;
    }

    int win_h = 0;
    if (args && args->ui_context) {
        sol_ui_system_window_size((const SolUISystem *)args->ui_context, NULL, &win_h);
    }
    if (win_h <= 0) win_h = 600;

    /* `rendered` is what we actually emit (slightly more than fits, so
       the parent always looks full). `viewport` is what the user can
       actually see and is used for scrollbar thumb math so the thumb
       has the correct length relative to the file. */
    const int rendered = sol_text_visible_lines(win_h);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;

    const int total   = (int)ts->line_count;
    int max_top = total > viewport ? total - viewport : 0;
    if (ts->scroll_top < 0)        ts->scroll_top = 0;
    if (ts->scroll_top > max_top)  ts->scroll_top = max_top;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "buffer-scroll-row",
    });

    /* --- Left gutter: line numbers ---------------------------------- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "buffer-gutter-col",
    });
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = ts->scroll_top + i;
        if (line_idx >= total) {
            ca_div_begin(&(Ca_DivDesc){ .style = "buffer-gutter-line-empty" });
            ca_div_end();
            continue;
        }
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%d", line_idx + 1);
        /* Causality copies the `text` pointer; we need stable storage
           per emission. Use a small per-frame static ring of strings
           so each row gets a unique buffer. */
        static char ring[128][16];
        static int  ring_cursor = 0;
        const int   slot = ring_cursor++ & 127;
        memcpy(ring[slot], num_buf, sizeof(num_buf));
        ca_text(&(Ca_TextDesc){
            .text  = ring[slot],
            .style = "buffer-gutter-line",
        });
    }
    ca_div_end();   /* buffer-gutter-col */

    /* --- Text column: one ca_text per visible line ---------------- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "buffer-text-col",
    });
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = ts->scroll_top + i;
        if (line_idx >= total) {
            ca_div_begin(&(Ca_DivDesc){ .style = "buffer-line-empty" });
            ca_div_end();
            continue;
        }
        const char *line = ts->lines[line_idx];
        /* causality treats NULL as empty-string; keep its single-line
           layout cell to preserve scroll alignment. */
        ca_text(&(Ca_TextDesc){
            .text  = (line && *line) ? line : " ",
            .style = "buffer-line",
        });
    }
    ca_div_end();   /* buffer-text-col */

    /* --- Custom scrollbar (only when the file overflows) ----------- */
    if (total > viewport && max_top > 0) {
        const float track_h     = (float)(viewport * SOL_TEXT_LINE_HEIGHT_PX);
        float thumb_h           = track_h * (float)viewport / (float)total;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        const float free_h      = track_h - thumb_h;
        const float top_spacer  = free_h * (float)ts->scroll_top / (float)max_top;

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "buffer-scrollbar",
        });
        /* Causality treats `.height == 0` as auto, which makes a flex
           child grow to fill — that would push the thumb to the bottom
           even when the user is at the top of the file. Skip the spacer
           entirely when its computed height is effectively zero. */
        if (top_spacer >= 0.5f) {
            ca_div_begin(&(Ca_DivDesc){
                .style  = "buffer-scrollbar-spacer",
                .height = top_spacer,
            });
            ca_div_end();
        }
        ca_div_begin(&(Ca_DivDesc){
            .style  = args && args->is_active
                          ? "buffer-scrollbar-thumb buffer-scrollbar-thumb-active"
                          : "buffer-scrollbar-thumb",
            .height = thumb_h,
        });
        ca_div_end();
        ca_div_end();   /* buffer-scrollbar */
    }

    ca_div_end();   /* buffer-scroll-row */
}

/* Split `text` (heap-owned) into lines in-place: replace every '\n'
   with '\0' and build a heap-allocated array of line pointers. On
   success the state takes ownership of `text` and the new array. */
static bool sol_text_buffer_build_lines(SolTextBufferState *state, char *text, size_t len)
{
    /* Always at least one line, even for an empty file. */
    size_t line_count = 1u;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') line_count++;
    }
    /* Trailing newline shouldn't create a phantom blank line. */
    if (len > 0u && text[len - 1u] == '\n' && line_count > 1u) line_count--;

    char **lines = (char **)calloc(line_count, sizeof(char *));
    if (!lines) return false;

    size_t idx = 0u;
    char *cursor = text;
    for (size_t i = 0; i < len && idx < line_count; ++i) {
        if (text[i] == '\n') {
            text[i] = '\0';
            lines[idx++] = cursor;
            cursor = &text[i + 1u];
        }
    }
    if (idx < line_count) {
        lines[idx++] = cursor;
    }

    state->text       = text;
    state->lines      = lines;
    state->line_count = line_count;
    state->scroll_top = 0;
    return true;
}

static SolBufferId sol_create_text_buffer(SolBufferSystem *buffers, const char *name, const char *text, const char *source_path)
{
    if (!buffers) {
        return 0u;
    }

    SolTextBufferState *state = (SolTextBufferState *)calloc(1u, sizeof(SolTextBufferState));
    if (!state) {
        return 0u;
    }

    /* Take a private heap copy of `text` so we can mutate it in place
       (replacing newlines with NUL terminators while building lines). */
    const size_t len = text ? strlen(text) : 0u;
    char *owned = (char *)malloc(len + 1u);
    if (!owned) {
        free(state);
        return 0u;
    }
    if (len > 0u) memcpy(owned, text, len);
    owned[len] = '\0';

    if (!sol_text_buffer_build_lines(state, owned, len)) {
        free(owned);
        free(state);
        return 0u;
    }

    if (source_path) {
        state->source_path = sol_strdup(source_path);
        /* Allocation failure here is non-fatal — the buffer is still
           usable, just can't be matched for dedupe. */
    }

    SolBufferDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = name;
    desc.kind = SOL_BUFFER_KIND_TEXT;
    desc.state = state;
    desc.ops.destroy = sol_text_buffer_destroy;
    desc.ops.render = sol_text_buffer_render;

    const SolBufferId id = sol_buffer_create(buffers, &desc);
    if (id == 0u) {
        sol_text_buffer_destroy(state);
    }

    return id;
}

/* Find an existing text buffer whose on-disk source path matches.
   Returns 0 when no match. Used to dedupe file opens so clicking the
   same file in the tree (or via the picker) reuses the existing tab
   instead of creating a new one — VS Code / Neovim style. */
static SolBufferId sol_find_text_buffer_by_path(SolBufferSystem *buffers, const char *path)
{
    if (!buffers || !path) return 0u;
    const size_t total = sol_buffer_count(buffers);
    for (size_t i = 0u; i < total; ++i) {
        const SolBufferId id = sol_buffer_at(buffers, i);
        if (id == 0u) continue;
        SolBuffer *buf = sol_buffer_get(buffers, id);
        if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) continue;
        const SolTextBufferState *ts =
            (const SolTextBufferState *)sol_buffer_state(buf);
        if (!ts || !ts->source_path) continue;
        if (strcmp(ts->source_path, path) == 0) return id;
    }
    return 0u;
}

/* Open `path` as a text buffer focused in the active leaf. If a buffer
 * for this path already exists, reuse it instead of creating a new one.
 * Returns true on success. */
static bool sol_open_path_in_active_leaf(SolBufferSystem *buffers, const char *path)
{
    if (!buffers || !path) return false;

    const SolBufferId existing = sol_find_text_buffer_by_path(buffers, path);
    if (existing != 0u) {
        return sol_buffer_set_active_leaf_buffer(buffers, existing);
    }

    size_t len = 0u;
    char *contents = sol_read_file_to_string(path, &len);
    if (!contents) {
        fprintf(stderr, "sol: cannot open '%s'\n", path);
        return false;
    }

    SolBufferId id = sol_create_text_buffer(buffers, sol_basename(path), contents, path);
    free(contents);
    if (id == 0u) {
        fprintf(stderr, "sol: failed to create buffer for '%s'\n", path);
        return false;
    }
    if (!sol_buffer_set_active_leaf_buffer(buffers, id)) {
        fprintf(stderr, "sol: failed to focus buffer for '%s'\n", path);
    }
    return true;
}

/* Bridge: SolUIFileOpenFn signature → buffer system. */
static bool sol_on_tree_file_open(const char *path, void *user_data)
{
    SolBufferSystem *buffers = (SolBufferSystem *)user_data;
    return sol_open_path_in_active_leaf(buffers, path);
}

/* File-picker callbacks. These run from inside a click handler in the
 * picker window's UI; the picker handle reaps itself on the next tick. */
static void sol_on_picker_file_chosen(const char *path, void *user_data)
{
    if (!path) return;   /* user cancelled */
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app) return;
    if (sol_open_path_in_active_leaf(app->buffers, path) && app->ui) {
        /* Tell the buffer area to rebuild so the newly-opened file
           appears immediately instead of after some unrelated update. */
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_picker_folder_chosen(const char *path, void *user_data)
{
    if (!path) return;   /* user cancelled */
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->ui) return;
    if (!sol_ui_system_set_file_tree_root(app->ui, path)) {
        fprintf(stderr, "sol: cannot open directory '%s'\n", path);
    }
}

/* Title-bar menu trampolines: open a picker in the matching mode. */
static void sol_on_menu_open_file(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance,
                         SOL_FILE_PICKER_FILE,
                         NULL,
                         sol_on_picker_file_chosen,
                         app);
}

static void sol_on_menu_open_folder(void *user_data)
{
    SolAppContext *app = (SolAppContext *)user_data;
    if (!app || !app->instance) return;
    sol_file_picker_open(app->instance,
                         SOL_FILE_PICKER_FOLDER,
                         NULL,
                         sol_on_picker_folder_chosen,
                         app);
}

static SolModifierMask sol_modifiers_from_ca(int mods)
{
    SolModifierMask out = SOL_MOD_NONE;
    if ((mods & 0x0001) != 0) {
        out |= SOL_MOD_SHIFT;
    }
    if ((mods & 0x0002) != 0) {
        out |= SOL_MOD_CTRL;
    }
    if ((mods & 0x0004) != 0) {
        out |= SOL_MOD_ALT;
    }
    if ((mods & 0x0008) != 0) {
        out |= SOL_MOD_SUPER;
    }
    return out;
}

static bool sol_on_startup_event(const SolEvent *event, void *user_data)
{
    (void)user_data;

    if (!event || !event->payload || event->payload_size != sizeof(SolStartupPayload)) {
        return false;
    }

    const SolStartupPayload *payload = (const SolStartupPayload *)event->payload;
    printf(
        "[sol] startup: workers=%u plugins=%u warmup=%llu input=%s\n",
        payload->worker_count,
        payload->loaded_plugins,
        (unsigned long long)payload->warmup_checksum,
        payload->input_binding_active ? "ready" : "missing"
    );

    return false;
}

static void sol_warmup_range(uint32_t begin, uint32_t end, void *user_data)
{
    SolWarmupContext *context = (SolWarmupContext *)user_data;
    uint64_t local_sum = 0u;

    for (uint32_t i = begin; i < end; ++i) {
        local_sum += ((uint64_t)i * 2654435761ull) ^ ((uint64_t)i >> 3u);
    }

    atomic_fetch_add_explicit(&context->checksum, local_sum, memory_order_relaxed);
}

static void sol_on_ca_key(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->key.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_KEY_UP;
    } else if (ev->key.action == CA_PRESS || ev->key.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_KEY_DOWN;
    } else {
        return;
    }

    input_event.data.key.key = (SolKeyCode)ev->key.key;
    input_event.data.key.modifiers = sol_modifiers_from_ca(ev->key.mods);
    input_event.data.key.repeated = (ev->key.action == CA_REPEAT);

    sol_ui_system_handle_input_event(app->ui, &input_event);
    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_char(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_TEXT_INPUT;
    input_event.data.text.codepoint = ev->character.codepoint;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_button(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    if (ev->mouse_button.action == CA_RELEASE) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_UP;
    } else if (ev->mouse_button.action == CA_PRESS || ev->mouse_button.action == CA_REPEAT) {
        input_event.type = SOL_INPUT_EVENT_MOUSE_DOWN;
    } else {
        return;
    }

    input_event.data.mouse_button.button = (SolMouseButton)ev->mouse_button.button;
    input_event.data.mouse_button.modifiers = sol_modifiers_from_ca(ev->mouse_button.mods);
    input_event.data.mouse_button.repeated = (ev->mouse_button.action == CA_REPEAT);

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_move(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    /* Cache for hit-testing in scroll handlers. */
    app->mouse_x = ev->mouse_pos.x;
    app->mouse_y = ev->mouse_pos.y;

    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_MOVE;
    input_event.data.mouse_move.x = ev->mouse_pos.x;
    input_event.data.mouse_move.y = ev->mouse_pos.y;
    input_event.data.mouse_move.delta_x = 0.0;
    input_event.data.mouse_move.delta_y = 0.0;

    sol_input_system_process_event(app->input, &input_event);
}

static void sol_on_ca_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    SolInputEvent input_event;
    memset(&input_event, 0, sizeof(input_event));

    input_event.type = SOL_INPUT_EVENT_MOUSE_SCROLL;
    input_event.data.mouse_scroll.x = (float)ev->mouse_scroll.dx;
    input_event.data.mouse_scroll.y = (float)ev->mouse_scroll.dy;

    sol_input_system_process_event(app->input, &input_event);

    /* Route the wheel to the pane under the cursor — even if it isn't
       the keyboard-focused one. This matches VS Code / browser behaviour
       and keeps mouse interaction natural. Falls back to the active
       leaf when the cursor isn't over a pane. */
    if (!app->buffers || !app->ui) return;

    int win_w = 0, win_h = 0;
    sol_ui_system_window_size(app->ui, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    const float title_h  = (float)sol_ui_system_title_bar_height(app->ui);
    const float status_h = (float)sol_ui_system_status_bar_height(app->ui);
    const float tree_w   = (float)sol_ui_system_tree_panel_width(app->ui);
    const float buf_x = tree_w;
    const float buf_y = title_h;
    const float buf_w = (float)win_w - tree_w;
    const float buf_h = (float)win_h - title_h - status_h;

    SolBufferId target_buffer = 0u;
    const double mx = app->mouse_x, my = app->mouse_y;
    if (mx >= buf_x && mx <= buf_x + buf_w &&
        my >= buf_y && my <= buf_y + buf_h)
    {
        SolBufferNodeId leaf = sol_buffer_leaf_at_point(
            app->buffers, buf_x, buf_y, buf_w, buf_h,
            /* split-bar size — keep in sync with workspace.c */ 1.0f,
            (float)mx, (float)my);
        if (leaf != 0u) {
            target_buffer = sol_buffer_leaf_buffer(app->buffers, leaf);
        }
    }
    if (target_buffer == 0u) {
        target_buffer = sol_buffer_active_buffer(app->buffers);
    }
    if (target_buffer == 0u) return;

    SolBuffer *buf = sol_buffer_get(app->buffers, target_buffer);
    if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) return;
    SolTextBufferState *ts = (SolTextBufferState *)sol_buffer_state(buf);
    if (!ts || ts->line_count == 0u) return;

    const int rendered = sol_text_visible_lines(win_h);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;
    const int total   = (int)ts->line_count;
    const int max_top = total > viewport ? total - viewport : 0;

    /* Natural scrolling: dy>0 scrolls content up (i.e. moves view down). */
    int delta = (int)(-ev->mouse_scroll.dy * 3.0);
    if (delta == 0) delta = ev->mouse_scroll.dy > 0.0 ? -1 : (ev->mouse_scroll.dy < 0.0 ? 1 : 0);

    int new_top = ts->scroll_top + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;

    if (new_top != ts->scroll_top) {
        ts->scroll_top = new_top;
        sol_ui_system_invalidate_buffer_area(app->ui);
    }
}

static void sol_on_ca_window_close(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_close(app->ui, ev->window);
}

static void sol_on_ca_window_resize(const Ca_Event *ev, void *user_data)
{
    if (!ev || !user_data) {
        return;
    }

    SolAppContext *app = (SolAppContext *)user_data;
    sol_ui_system_on_window_resize(app->ui, ev->resize.width, ev->resize.height);
}

int main(int argc, char **argv)
{
    SolAppContext app;
    memset(&app, 0, sizeof(app));

    SolSystemConfig system_config = sol_system_config_default();
    app.systems = sol_system_manager_create(&system_config);
    if (!app.systems) {
        fprintf(stderr, "Failed to create system manager\n");
        return 1;
    }

    app.events = sol_system_events(app.systems);
    app.buffers = sol_system_buffers(app.systems);
    app.jobs = sol_system_jobs(app.systems);
    app.input = sol_system_input(app.systems);

    /* No welcome / placeholder buffer: launching with neither a file
       nor a directory should leave the workspace empty (no tabs, no
       phantom "main" buffer). The first buffer is created lazily — by
       the CLI file path below, by File → Open, or by clicking a file
       in the tree panel. */

    /* CLI: `./sol <path>` opens the given file or directory.
     *   - file: load it into a fresh text buffer
     *   - dir : mount it as the file-tree root
     * The file-tree root is set after the UI system is created (below). */
    const char *cli_path = (argc >= 2 && argv[1] && argv[1][0] != '\0') ? argv[1] : NULL;
    bool cli_is_dir = false;
    if (cli_path) {
        struct stat st;
        if (stat(cli_path, &st) != 0) {
            fprintf(stderr, "sol: cannot stat '%s'\n", cli_path);
            sol_system_manager_destroy(app.systems);
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            cli_is_dir = true;
        } else if (!sol_open_path_in_active_leaf(app.buffers, cli_path)) {
            sol_system_manager_destroy(app.systems);
            return 1;
        }
    }

    app.startup_token = sol_event_bus_subscribe(app.events, &(SolEventSubscriptionDesc){
        .event_name = "core.startup",
        .priority = 100,
        .handler = sol_on_startup_event,
        .user_data = NULL,
    });

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .font_size_px         = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT,
    });
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        sol_system_manager_destroy(app.systems);
        return 1;
    }
    app.instance = instance;

    app.ui = sol_ui_system_create(instance, app.buffers);
    if (!app.ui) {
        fprintf(stderr, "Failed to create UI system\n");
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    /* File tree wiring: route clicks to a buffer-create + focus path,
     * and mount the root if a directory was given on the CLI. */
    sol_ui_system_set_file_open_callback(app.ui, sol_on_tree_file_open, app.buffers);
    if (cli_is_dir && cli_path) {
        if (!sol_ui_system_set_file_tree_root(app.ui, cli_path)) {
            fprintf(stderr, "sol: cannot open directory '%s'\n", cli_path);
        }
    }

    /* Title-bar File menu: Open File... / Open Folder... */
    sol_ui_system_install_menu(app.ui,
                               sol_on_menu_open_file,
                               sol_on_menu_open_folder,
                               &app);

    static const SolKeyCode flow_editor_save[] = { 'F', 'S' };
    static const SolKeyCode flow_workspace_split_vertical[] = { 'W', 'V' };
    static const SolKeyCode flow_workspace_split_horizontal[] = { 'W', 'H' };
    static const SolKeyCode flow_workspace_focus_next[] = { 'W', 'N' };
    static const SolKeyCode flow_buffer_next[] = { 'B', 'D' };
    static const SolKeyCode flow_buffer_prev[] = { 'B', 'A' };

    const bool save_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "editor.save",
        .label = "Save",
        .sequence = flow_editor_save,
        .sequence_length = 2u,
        .key = 'S',
        .callback = sol_ui_system_on_save_action,
        .user_data = app.ui,
    });

    const bool split_vertical_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.vertical",
        .label = "Split Vertical",
        .sequence = flow_workspace_split_vertical,
        .sequence_length = 2u,
        .key = 'V',
        .callback = sol_ui_system_on_split_vertical_action,
        .user_data = app.ui,
    });

    const bool split_horizontal_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.split.horizontal",
        .label = "Split Horizontal",
        .sequence = flow_workspace_split_horizontal,
        .sequence_length = 2u,
        .key = 'H',
        .callback = sol_ui_system_on_split_horizontal_action,
        .user_data = app.ui,
    });

    const bool focus_next_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "workspace.focus.next",
        .label = "Focus Next Pane",
        .sequence = flow_workspace_focus_next,
        .sequence_length = 2u,
        .key = 'N',
        .callback = sol_ui_system_on_focus_next_action,
        .user_data = app.ui,
    });

    const bool buffer_next_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "buffer.next",
        .label = "Next Buffer",
        .sequence = flow_buffer_next,
        .sequence_length = 2u,
        .key = 'D',
        .callback = sol_ui_system_on_buffer_next_action,
        .user_data = app.ui,
    });

    const bool buffer_prev_flow = sol_ui_system_register_command_flow(app.ui, &(SolCommandFlowDesc){
        .action = "buffer.prev",
        .label = "Previous Buffer",
        .sequence = flow_buffer_prev,
        .sequence_length = 2u,
        .key = 'A',
        .callback = sol_ui_system_on_buffer_prev_action,
        .user_data = app.ui,
    });

    app.command_flows_ready = save_flow && split_vertical_flow && split_horizontal_flow
                             && focus_next_flow && buffer_next_flow && buffer_prev_flow;

    ca_event_set_handler(instance, CA_EVENT_KEY, sol_on_ca_key, &app);
    ca_event_set_handler(instance, CA_EVENT_CHAR, sol_on_ca_char, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON, sol_on_ca_mouse_button, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE, sol_on_ca_mouse_move, &app);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL, sol_on_ca_mouse_scroll, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE, sol_on_ca_window_close, &app);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, sol_on_ca_window_resize, &app);

    Ca_Window *window = sol_ui_system_primary_window(app.ui);
    if (!window) {
        fprintf(stderr, "Failed to access primary window\n");
        sol_ui_system_destroy(app.ui);
        ca_instance_destroy(instance);
        sol_system_manager_destroy(app.systems);
        return 1;
    }

    if (!sol_system_register_service(app.systems, "ca.instance", instance, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.instance service\n");
    }
    if (!sol_system_register_service(app.systems, "ca.window.primary", window, NULL, NULL)) {
        fprintf(stderr, "[sol] warning: failed to register ca.window.primary service\n");
    }

    SolWarmupContext warmup = { 0 };
    bool warmup_ok = sol_job_system_parallel_for(app.jobs, 100000u, 256u, sol_warmup_range, &warmup);

    const uint32_t loaded_plugins = (uint32_t)sol_system_load_plugins_from_directory(app.systems, NULL);

    const SolStartupPayload startup = {
        .worker_count = sol_job_system_worker_count(app.jobs),
        .loaded_plugins = loaded_plugins,
        .warmup_checksum = warmup_ok ? atomic_load_explicit(&warmup.checksum, memory_order_relaxed) : 0u,
        .input_binding_active = app.command_flows_ready,
    };

    sol_event_bus_post(app.events, &(SolEventDesc){
        .event_name = "core.startup",
        .payload = &startup,
        .payload_size = sizeof(startup),
        .sender = app.systems,
        .flags = SOL_EVENT_FLAG_NONE,
    });
    sol_system_pump_events(app.systems, 16u);

    for (;;) {
        sol_system_begin_frame(app.systems);
        if (!ca_instance_tick(instance)) {
            break;
        }
        sol_system_pump_events(app.systems, 128u);
        sol_system_end_frame(app.systems);
    }

    if (app.startup_token != 0u) {
        sol_event_bus_unsubscribe(app.events, app.startup_token);
    }

    sol_system_unregister_service(app.systems, "ca.window.primary");
    sol_system_unregister_service(app.systems, "ca.instance");

    sol_ui_system_destroy(app.ui);

    ca_instance_destroy(instance);
    sol_system_manager_destroy(app.systems);
    return 0;
}
