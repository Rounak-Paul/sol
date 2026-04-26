// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol — code editor on causality.

#include <causality.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_editor.h"
#include "sol_input.h"
#include "ui/sol_style.h"

typedef struct SolApp {
    Ca_Instance   *inst;
    Ca_Window     *win;
    Ca_Stylesheet *css;
    SolEditor     *ed;
    Ca_Div        *root;   /* the rebuildable root div */
} SolApp;

/* ---- Reactive root builder ---- */

static void editor_builder(Ca_Div *div, void *user)
{
    (void)div;
    SolApp *a = (SolApp *)user;
    sol_editor_render(a->ed, a->win);
}

/* ---- Causality event glue ---- */

static void on_key(const Ca_Event *ev, void *user)
{
    SolApp *a = (SolApp *)user;
    if (!ev || !a) return;
    if (ev->key.action == 0 /* CA_RELEASE */) return;
    SolInputEvent ie = {
        .kind   = SOL_INPUT_KEY_DOWN,
        .key    = (SolKeyCode)ev->key.key,
        .mods   = sol_modifiers_from_ca(ev->key.mods),
        .repeat = (ev->key.action == 2 /* CA_REPEAT */),
    };
    if (sol_editor_input(a->ed, &ie) && a->root)
        ca_div_invalidate(a->root);
}

static void on_char(const Ca_Event *ev, void *user)
{
    SolApp *a = (SolApp *)user;
    if (!ev || !a) return;
    SolInputEvent ie = { .kind = SOL_INPUT_TEXT, .codepoint = ev->character.codepoint };
    if (sol_editor_input(a->ed, &ie) && a->root)
        ca_div_invalidate(a->root);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    SolApp app = (SolApp){0};

    app.inst = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Sol",
        .prefer_dedicated_gpu = true,
        .font_size_px         = 13.0f,
    });
    if (!app.inst) {
        fprintf(stderr, "[sol] ca_instance_create failed\n");
        return 1;
    }

    app.css = ca_css_parse(SOL_STYLESHEET);
    if (app.css) ca_instance_set_stylesheet(app.inst, app.css);

    app.win = ca_window_create(app.inst, &(Ca_WindowDesc){
        .title  = "Sol",
        .width  = 1280,
        .height = 800,
    });
    if (!app.win) {
        fprintf(stderr, "[sol] ca_window_create failed\n");
        ca_css_destroy(app.css);
        ca_instance_destroy(app.inst);
        return 1;
    }

    app.ed = sol_editor_create();
    if (!app.ed) { fprintf(stderr, "[sol] editor create failed\n"); return 1; }

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) sol_editor_open_path(app.ed, argv[i]);
    } else {
        sol_editor_open_scratch(app.ed, "[scratch]");
    }

    /* Build a single rebuildable root div. The builder fn is invoked by the
       runtime whenever ca_div_invalidate(root) is called. */
    ca_ui_begin(app.win, &(Ca_DivDesc){ .style = "root", .direction = 1 });
        app.root = ca_div_begin(&(Ca_DivDesc){ .style = "editor-shell", .direction = 1 });
        ca_div_end();
    ca_ui_end();

    ca_div_set_builder(app.root, editor_builder, &app);
    ca_div_invalidate(app.root);

    ca_event_set_handler(app.inst, CA_EVENT_KEY,  on_key,  &app);
    ca_event_set_handler(app.inst, CA_EVENT_CHAR, on_char, &app);

    while (ca_window_is_open(app.win)) {
        if (!ca_instance_tick(app.inst)) break;
    }

    sol_editor_destroy(app.ed);
    ca_window_destroy(app.win);
    ca_css_destroy(app.css);
    ca_instance_destroy(app.inst);
    return 0;
}
