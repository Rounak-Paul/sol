// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_editor.h — editor model, no vim modes.
//
// The editor is always editable (typing inserts into the active buffer).
// Commands are bound to a leader-key tree, navigated which-key style:
// hold the leader (Cmd on macOS, Ctrl elsewhere) and tap a sequence of
// letters. After the first letter, a popup shows the next available
// nodes. A leaf fires its action; Esc cancels.

#ifndef SOL_EDITOR_H
#define SOL_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolTextBuffer SolTextBuffer;
typedef struct Ca_Window     Ca_Window;
typedef struct Ca_Instance   Ca_Instance;

#define SOL_EDITOR_MAX_DOCS    64
#define SOL_EDITOR_MAX_PATH   512
#define SOL_EDITOR_FLOW_MAX    16  /* max chord depth */

typedef struct SolDoc {
    char            path[SOL_EDITOR_MAX_PATH]; /* "" = scratch */
    char            display_name[64];
    SolTextBuffer  *text;
    size_t          cursor;       /* byte offset */
    size_t          scroll_line;  /* top visible line */
    bool            in_use;
} SolDoc;

typedef struct SolEditor SolEditor;

/* Action callback for a leaf node. */
typedef void (*SolFlowAction)(SolEditor *ed);

/* One row in the static command flow table. `path` is a sequence of
   uppercase ASCII letters (e.g. "F", "FS"). Interior nodes have
   action == NULL. */
typedef struct SolFlowEntry {
    const char    *path;
    const char    *label;
    SolFlowAction  action;
} SolFlowEntry;

/* ---- Lifecycle ---- */

SolEditor *sol_editor_create(void);
void       sol_editor_destroy(SolEditor *ed);

/* ---- Documents ---- */

int  sol_editor_open_path   (SolEditor *ed, const char *path);
int  sol_editor_open_scratch(SolEditor *ed, const char *display_name);
bool sol_editor_close_doc   (SolEditor *ed, int index);
bool sol_editor_save_doc    (SolEditor *ed, int index);
void sol_editor_set_active  (SolEditor *ed, int index);

int           sol_editor_active_index(const SolEditor *ed);
size_t        sol_editor_doc_count   (const SolEditor *ed);
const SolDoc *sol_editor_doc         (const SolEditor *ed, int index);
SolDoc       *sol_editor_doc_mut     (SolEditor       *ed, int index);

/* ---- Command flow (which-key) ---- */

const char *sol_editor_flow_prefix(const SolEditor *ed); /* current chord, e.g. "F" */

/* ---- Input + render ---- */

bool sol_editor_input (SolEditor *ed, const SolInputEvent *ev);
void sol_editor_render(SolEditor *ed, Ca_Window *win);

#ifdef __cplusplus
}
#endif

#endif
