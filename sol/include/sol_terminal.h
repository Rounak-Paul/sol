// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_terminal.h — Sol integrated terminal emulator: VT state machine,
 * cell model, PTY backend, and multi-tab manager.
 *
 * Layering:
 *   SolTerminal         single terminal session (PTY + VT parser + cell grid)
 *   SolTerminalManager  owns N sessions; tracks focus, visibility, position
 *
 * Threading:
 *   Each SolTerminal spins a reader thread that blocks on the PTY master fd
 *   and deposits raw bytes into a 64KB ring buffer.  The main thread drains
 *   the ring each frame via sol_terminal_drain(), runs the VT parser, and
 *   marks dirty lines.  ca_instance_wake() is called from the reader thread
 *   after each deposit so the UI wakes and repaints.
 */

#ifndef SOL_TERMINAL_H
#define SOL_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Cell model                                                          */
/* ================================================================== */

typedef enum SolTermColorMode {
    SOL_TERM_COLOR_DEFAULT  = 0,   /* use terminal default fg or bg */
    SOL_TERM_COLOR_INDEXED,        /* 256-color palette, index 0-255 */
    SOL_TERM_COLOR_RGB,            /* 24-bit true color */
} SolTermColorMode;

typedef struct SolTermColor {
    SolTermColorMode mode;
    union {
        uint8_t index;                      /* SOL_TERM_COLOR_INDEXED */
        struct { uint8_t r, g, b; } rgb;    /* SOL_TERM_COLOR_RGB     */
    };
} SolTermColor;

/* SGR attribute flags (bitmask). */
#define SOL_TERM_ATTR_BOLD      (1u << 0)
#define SOL_TERM_ATTR_DIM       (1u << 1)
#define SOL_TERM_ATTR_ITALIC    (1u << 2)
#define SOL_TERM_ATTR_UNDERLINE (1u << 3)
#define SOL_TERM_ATTR_BLINK     (1u << 4)
#define SOL_TERM_ATTR_REVERSE   (1u << 5)
#define SOL_TERM_ATTR_INVISIBLE (1u << 6)
#define SOL_TERM_ATTR_STRIKE    (1u << 7)
#define SOL_TERM_ATTR_WIDE      (1u << 8)   /* double-width CJK character */
#define SOL_TERM_ATTR_WIDE_TAIL (1u << 9)   /* trailing cell of a wide pair; not rendered */

/* Atomic unit of the terminal grid. */
typedef struct SolTermCell {
    uint32_t     codepoint;   /* Unicode codepoint; 0 = empty (renders as space) */
    SolTermColor fg;
    SolTermColor bg;
    uint16_t     attrs;       /* SOL_TERM_ATTR_* bitmask */
} SolTermCell;

/* One row of terminal cells. */
typedef struct SolTermLine {
    SolTermCell *cells;       /* `cols`-element heap array */
    int          cols;        /* column count, matches terminal width */
    bool         dirty;       /* line changed since last render */
} SolTermLine;

/* Maximum number of terminal tabs per manager. */
#define SOL_TERM_MAX_TABS 16

/* ================================================================== */
/* Layout position                                                     */
/* ================================================================== */

typedef enum SolTerminalPosition {
    SOL_TERMINAL_POSITION_BOTTOM = 0, /* horizontal strip below buffer area */
    SOL_TERMINAL_POSITION_RIGHT,      /* vertical strip right of buffer area */
} SolTerminalPosition;

/* ================================================================== */
/* Opaque types                                                        */
/* ================================================================== */

typedef struct SolTerminal        SolTerminal;
typedef struct SolTerminalManager SolTerminalManager;

/* Causality instance pointer (forward-decl; terminal.c includes causality.h). */
typedef struct Ca_Instance Ca_Instance;

/* ================================================================== */
/* Manager lifecycle                                                   */
/* ================================================================== */

/*
 * Create a terminal manager.
 *
 * instance  Causality instance used for ca_instance_wake() from reader threads.
 * Returns   Heap-allocated manager, or NULL on failure.
 */
SolTerminalManager *sol_terminal_manager_create(Ca_Instance *instance);

/*
 * Destroy the manager and all owned terminal sessions.
 *
 * mgr  The manager to destroy (safe to call with NULL).
 */
void sol_terminal_manager_destroy(SolTerminalManager *mgr);

/* ================================================================== */
/* Tab management                                                      */
/* ================================================================== */

/*
 * Open a new terminal tab, launching $SHELL.
 *
 * mgr   The manager.
 * cwd   Working directory for the shell, or NULL to inherit the process cwd.
 * Returns  The new terminal, or NULL on PTY/fork failure.
 */
SolTerminal *sol_terminal_manager_new_tab(SolTerminalManager *mgr,
                                          const char *cwd);

/*
 * Kill and remove the active terminal tab.
 *
 * mgr  The manager.
 */
void sol_terminal_manager_close_active(SolTerminalManager *mgr);

/* Advance to the next tab (wraps around). */
void sol_terminal_manager_next_tab(SolTerminalManager *mgr);

/* Advance to the previous tab (wraps around). */
void sol_terminal_manager_prev_tab(SolTerminalManager *mgr);

/* Returns the active terminal, or NULL when no tabs exist. */
SolTerminal *sol_terminal_manager_active(const SolTerminalManager *mgr);

/* Returns the number of open terminal tabs. */
size_t sol_terminal_manager_count(const SolTerminalManager *mgr);

/* Returns the zero-based index of the active tab. */
size_t sol_terminal_manager_active_index(const SolTerminalManager *mgr);

/* Returns the terminal at `index`, or NULL when out of range. */
SolTerminal *sol_terminal_manager_at(const SolTerminalManager *mgr, size_t index);

/* ================================================================== */
/* Visibility / focus / layout                                         */
/* ================================================================== */

/* Returns true while the terminal panel is visible. */
bool sol_terminal_manager_visible(const SolTerminalManager *mgr);

/*
 * Show or hide the terminal panel.
 *
 * mgr      The manager.
 * visible  Desired visibility.
 */
void sol_terminal_manager_set_visible(SolTerminalManager *mgr, bool visible);

/* Returns true while the terminal panel has keyboard focus. */
bool sol_terminal_manager_focused(const SolTerminalManager *mgr);

/*
 * Set terminal focus state.  Unfocusing does NOT hide the panel.
 *
 * mgr      The manager.
 * focused  Desired focus state.
 */
void sol_terminal_manager_set_focused(SolTerminalManager *mgr, bool focused);

/* Returns the current layout position (bottom or right). */
SolTerminalPosition sol_terminal_manager_position(const SolTerminalManager *mgr);

/*
 * Change the layout position (triggers a workspace rebuild).
 *
 * mgr  The manager.
 * pos  New position.
 */
void sol_terminal_manager_set_position(SolTerminalManager *mgr,
                                       SolTerminalPosition pos);

/* Returns the split ratio for the terminal panel (0.0–1.0). */
float sol_terminal_manager_ratio(const SolTerminalManager *mgr);

/*
 * Set the split ratio for the terminal panel.
 *
 * mgr    The manager.
 * ratio  Panel fraction of the split dimension.
 */
void sol_terminal_manager_set_ratio(SolTerminalManager *mgr, float ratio);

/* ================================================================== */
/* Per-terminal drain (call from main thread each frame)              */
/* ================================================================== */

/*
 * Drain PTY output from the ring buffer, run it through the VT parser,
 * and update the cell grid.  Call once per frame from the UI thread.
 *
 * mgr  The manager.
 * Returns true if any terminal's state changed (caller should bump sig_terminal_rev).
 */
bool sol_terminal_manager_drain(SolTerminalManager *mgr);

/* ================================================================== */
/* Per-terminal operations                                             */
/* ================================================================== */

/*
 * Notify the terminal that its display area has been resized.
 * Sends TIOCSWINSZ (Unix) or ResizePseudoConsole (Windows) to the PTY.
 *
 * term  The terminal.
 * cols  New column count (must be >= 1).
 * rows  New row count (must be >= 1).
 */
void sol_terminal_resize(SolTerminal *term, int cols, int rows);

/*
 * Encode and send a key event to the terminal's PTY.
 *
 * term  The terminal.
 * key   SolKeyCode from the input system.
 * mods  Active modifier mask (SOL_MOD_*).
 */
void sol_terminal_send_key(SolTerminal *term, uint32_t key, uint8_t mods);

/*
 * Write raw UTF-8 bytes to the terminal's PTY (e.g. for clipboard paste).
 *
 * term  The terminal.
 * data  Bytes to write.
 * len   Number of bytes.
 */
void sol_terminal_send_text(SolTerminal *term, const char *data, size_t len);

/*
 * Paste clipboard text into the terminal, wrapping with bracketed-paste
 * sequences (\033[200~ / \033[201~) when the application has enabled
 * bracketed paste mode (XTerm ?2004).  Use this instead of send_text for
 * all user-initiated paste operations so applications like vim and bash
 * receive the correct framing.
 *
 * term  The terminal.
 * data  UTF-8 clipboard text to paste.
 * len   Number of bytes.
 */
void sol_terminal_paste(SolTerminal *term, const char *data, size_t len);

/* Returns true while the child process is still running. */
bool sol_terminal_is_alive(const SolTerminal *term);

/* Force-kill the child process (SIGKILL on Unix). */
void sol_terminal_kill(SolTerminal *term);

/* ================================================================== */
/* Cell grid query (call from the render thread)                       */
/* ================================================================== */

/* Returns the terminal's current column count. */
int sol_terminal_cols(const SolTerminal *term);

/* Returns the terminal's current row count. */
int sol_terminal_rows(const SolTerminal *term);

/* Returns the cursor column (0-based, viewport-relative). */
int sol_terminal_cursor_col(const SolTerminal *term);

/* Returns the cursor row (0-based, viewport-relative). */
int sol_terminal_cursor_row(const SolTerminal *term);

/*
 * Get the line visible at visual row `row` (0 = topmost visible row).
 *
 * Returns a const pointer into the terminal's internal storage (valid until
 * the next sol_terminal_drain call).  Returns NULL when `row` is out of range.
 *
 * When `view_scroll > 0`, rows 0..view_scroll-1 come from the scrollback;
 * rows view_scroll..rows-1 come from the viewport.
 */
const SolTermLine *sol_terminal_view_line(const SolTerminal *term, int row);

/* Returns the number of scrollback lines available above the viewport. */
int sol_terminal_scrollback_count(const SolTerminal *term);

/* Returns the current view scroll offset (0 = at bottom, k = k lines above viewport). */
int sol_terminal_view_scroll(const SolTerminal *term);

/*
 * Scroll the view.  Clamped to [0, scrollback_count].
 *
 * term    The terminal.
 * offset  New scroll offset (0 = viewport).
 */
void sol_terminal_set_view_scroll(SolTerminal *term, int offset);

/* Returns the current terminal title (from OSC 0/1/2), never NULL. */
const char *sol_terminal_title(const SolTerminal *term);

/* Returns true if the cursor should be visible at the current position. */
bool sol_terminal_cursor_visible(const SolTerminal *term);

/* ================================================================== */
/* Color conversion utility                                            */
/* ================================================================== */

/*
 * Convert a SolTermColor to a packed RGBA uint32 suitable for Causality.
 *
 * c      The terminal color to convert.
 * is_fg  True to resolve DEFAULT as the terminal foreground color;
 *        false to resolve it as the background color.
 * Returns Packed RGBA uint32 (same layout as ca_color output).
 */
uint32_t sol_term_color_to_rgba(const SolTermColor *c, bool is_fg);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TERMINAL_H */
