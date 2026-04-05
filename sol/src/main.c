#include <causality.h>
#include <stdio.h>
#include <string.h>

#include "sol_event.h"
#include "sol_buffer.h"
#include "sol_textarea.h"

/* ============================================================
   CSS
   ============================================================ */

static const char *g_css =
    /* Clip overflow for the textarea outer container. */
    ".sol-clip {"
    "  overflow: hidden;"
    "}"

    /* Dark editor root background. */
    ".sol-root {"
    "  background: #0d0d12;"
    "  flex-direction: column;"
    "}";

/* ============================================================
   APP CONTEXT
   ============================================================ */

typedef struct {
    Ca_Window    *window;
    Sol_Textarea *textarea;
} AppCtx;

static AppCtx g_app;

/* ============================================================
   PER-FRAME CALLBACK
   ============================================================ */

static void on_frame(void *user_data)
{
    AppCtx *ctx = (AppCtx *)user_data;
    sol_textarea_update(ctx->textarea);
}

/* ============================================================
   CAUSALITY → SOL EVENT BRIDGE
   ============================================================ */

static void on_ca_event(const Ca_Event *e, void *user_data)
{
    (void)user_data;

    switch (e->type) {
    case CA_EVENT_WINDOW_RESIZE:
        sol_event_emit(SOL_EVENT_WINDOW_RESIZE, &(Sol_ResizeEvent){
            .width  = e->resize.width,
            .height = e->resize.height,
        });
        break;

    case CA_EVENT_KEY:
        sol_event_emit(SOL_EVENT_KEY, &(Sol_KeyEvent){
            .key      = e->key.key,
            .scancode = e->key.scancode,
            .action   = e->key.action,
            .mods     = e->key.mods,
        });
        break;

    case CA_EVENT_CHAR:
        sol_event_emit(SOL_EVENT_CHAR, &(Sol_CharEvent){
            .codepoint = e->character.codepoint,
        });
        break;

    case CA_EVENT_MOUSE_BUTTON:
        sol_event_emit(SOL_EVENT_MOUSE_BUTTON, &(Sol_MouseButtonEvent){
            .button = e->mouse_button.button,
            .action = e->mouse_button.action,
            .mods   = e->mouse_button.mods,
        });
        break;

    case CA_EVENT_MOUSE_MOVE:
        sol_event_emit(SOL_EVENT_MOUSE_MOVE, &(Sol_MouseMoveEvent){
            .x = e->mouse_pos.x,
            .y = e->mouse_pos.y,
        });
        break;

    case CA_EVENT_MOUSE_SCROLL:
        sol_event_emit(SOL_EVENT_MOUSE_SCROLL, &(Sol_MouseScrollEvent){
            .dx = e->mouse_scroll.dx,
            .dy = e->mouse_scroll.dy,
        });
        break;

    default:
        break;
    }
}

/* ============================================================
   DEMO CONTENT
   ============================================================ */

/* Insert a few screens worth of demo text so scrolling can be tested. */
static void populate_demo_buffer(Sol_Buffer *buf)
{
    static const char *lines[] = {
        "// Sol — a hackable text editor",
        "// Navigate: arrows, hjkl (normal mode)",
        "// Edit:     i = insert, Esc = normal, o = new line",
        "//           x = delete char, backspace / delete",
        "",
        "#include <stdio.h>",
        "#include \"sol_buffer.h\"",
        "#include \"sol_event.h\"",
        "",
        "int main(void) {",
        "    Sol_Buffer *buf = sol_buffer_create();",
        "    if (!buf) return 1;",
        "",
        "    sol_buffer_insert(buf, 0, 0, \"hello, world\\n\", 13);",
        "",
        "    uint32_t line, col;",
        "    sol_buffer_cursor_get(buf, &line, &col);",
        "    printf(\"cursor at %u:%u\\n\", line, col);",
        "",
        "    sol_buffer_destroy(buf);",
        "    return 0;",
        "}",
        "",
        "// --- more demo lines ---",
        "// Line 24",
        "// Line 25",
        "// Line 26",
        "// Line 27",
        "// Line 28",
        "// Line 29",
        "// Line 30",
        "// Line 31",
        "// Line 32 — scroll down to test virtualisation",
        "// Line 33",
        "// Line 34",
        "// Line 35",
        "// Line 36",
        "// Line 37",
        "// Line 38",
        "// Line 39",
        "// Line 40",
    };

    /* Build content by appending lines via the public API. */
    size_t n = sizeof(lines) / sizeof(lines[0]);
    for (size_t i = 0; i < n; i++) {
        size_t ll = strlen(lines[i]);
        uint32_t cur_count = sol_buffer_line_count(buf);
        uint32_t last_line = cur_count - 1u;
        uint32_t last_len;
        sol_buffer_line(buf, last_line, &last_len);

        if (i == 0) {
            /* Insert text into the initial empty line. */
            if (ll > 0) sol_buffer_insert(buf, 0, 0, lines[i], (uint32_t)ll);
        } else {
            /* Append newline to end of last line, then insert text. */
            sol_buffer_insert(buf, last_line, last_len, "\n", 1u);
            uint32_t new_line = sol_buffer_line_count(buf) - 1u;
            if (ll > 0) sol_buffer_insert(buf, new_line, 0, lines[i], (uint32_t)ll);
        }
    }

    sol_buffer_cursor_set(buf, 0, 0);
}

/* ============================================================
   ENTRY POINT
   ============================================================ */

int main(void)
{
    sol_events_init();
    sol_buffers_init();

    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .font_size_px         = 14.0f,
    });
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        sol_buffers_shutdown();
        sol_events_shutdown();
        return 1;
    }

    const int WIN_W = 800;
    const int WIN_H = 600;

    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = "Sol",
        .width  = WIN_W,
        .height = WIN_H,
    });
    if (!window) {
        ca_instance_destroy(instance);
        sol_buffers_shutdown();
        sol_events_shutdown();
        return 1;
    }

    /* Apply stylesheet. */
    Ca_Stylesheet *sheet = ca_css_parse(g_css);
    if (sheet) ca_instance_set_stylesheet(instance, sheet);

    /* Register the causality → sol event bridge. */
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_ca_event, NULL);
    ca_event_set_handler(instance, CA_EVENT_KEY,           on_ca_event, NULL);
    ca_event_set_handler(instance, CA_EVENT_CHAR,          on_ca_event, NULL);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_BUTTON,  on_ca_event, NULL);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_MOVE,    on_ca_event, NULL);
    ca_event_set_handler(instance, CA_EVENT_MOUSE_SCROLL,  on_ca_event, NULL);

    /* Create and populate the initial buffer. */
    Sol_Buffer *buf = sol_buffer_create();
    populate_demo_buffer(buf);

    /* Create the textarea (starts focused). */
    Sol_Textarea *tv = sol_textarea_create(buf, (float)WIN_W, (float)WIN_H);
    sol_textarea_focus(tv);

    g_app.window   = window;
    g_app.textarea = tv;

    /* Register the per-frame rebuild callback. */
    ca_window_set_on_frame(window, on_frame, &g_app);

    /* Build the initial UI skeleton (outer container planted here once). */
    ca_ui_begin(window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "sol-root",
    });
    /* Fill the whole root; width=0, height=0 means fill parent. */
    sol_textarea_build(tv, 0.0f, 0.0f);
    ca_ui_end();

    sol_event_emit(SOL_EVENT_APP_INIT, NULL);

    while (ca_instance_tick(instance)) {
        sol_event_emit(SOL_EVENT_APP_TICK, NULL);
    }

    sol_event_emit(SOL_EVENT_APP_QUIT, NULL);

    /* Cleanup. */
    ca_window_set_on_frame(window, NULL, NULL);
    sol_textarea_destroy(tv);
    sol_buffer_destroy(buf);
    if (sheet) ca_css_destroy(sheet);
    ca_instance_destroy(instance);
    sol_buffers_shutdown();
    sol_events_shutdown();
    return 0;
}
