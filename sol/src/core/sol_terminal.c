// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_terminal.c — VT state machine, cell grid, PTY backend, reader thread.
 *
 * VT parser: Paul Williams state machine (vt100.net/emu/dec_ansi_parser).
 * Cell grid: scrollback ring + viewport grid + alt-screen.
 * PTY (Unix):  forkpty/posix_openpt + reader pthread.
 * PTY (Win):   ConPTY (CreatePseudoConsole) + reader thread.
 */

#include "sol_terminal.h"
#include "sol_input.h"      /* SOL_KEY_*, SOL_MOD_* constants */
#include "sol_threading.h"

#include <causality.h>

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Map a 0-5 index in the 6x6x6 color cube to a byte value. */
static uint8_t ansi_cube_to_byte(uint8_t v) { return v ? (uint8_t)(55 + v * 40) : 0u; }

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define SOL_TERM_DEFAULT_COLS       80
#define SOL_TERM_DEFAULT_ROWS       24
#define SOL_TERM_MAX_PARAMS         16
#define SOL_TERM_SCROLLBACK_MAX   5000
#define SOL_TERM_OUTPUT_RING_SIZE 524288   /* 512 KB — handles large cmatrix bursts */
#define SOL_TERM_TITLE_MAX          256
#define SOL_TERM_OSC_MAX            1024
/* SOL_TERM_MAX_TABS is defined in sol_terminal.h */

/* ================================================================== */
/* VT parser states                                                    */
/* ================================================================== */

typedef enum SolVtState {
    VT_GROUND = 0,
    VT_ESCAPE,
    VT_ESCAPE_INT,
    VT_CSI_ENTRY,
    VT_CSI_PARAM,
    VT_CSI_INT,
    VT_CSI_IGNORE,
    VT_DCS_ENTRY,
    VT_DCS_PARAM,
    VT_DCS_INT,
    VT_DCS_PASSTHROUGH,
    VT_DCS_IGNORE,
    VT_OSC_STRING,
    VT_SOS_PM_APC,
} SolVtState;

/* ================================================================== */
/* SolTerminal                                                         */
/* ================================================================== */

struct SolTerminal {
    Ca_Instance *instance;

    /* Viewport (active screen). */
    SolTermLine  screen[SOL_TERM_DEFAULT_ROWS + 64]; /* max rows supported */
    int          rows, cols;

    /* Scrollback ring buffer. */
    SolTermLine  scrollback[SOL_TERM_SCROLLBACK_MAX];
    int          scrollback_count;  /* lines currently stored */
    int          scrollback_head;   /* index of oldest line   */

    /* Alt-screen (allocated lazily on first ?1049h). */
    SolTermLine *alt_screen;        /* rows*cols grid, NULL until first use */
    int          alt_screen_rows;   /* row count at time alt_screen was allocated */
    bool         in_alt_screen;

    /* Saved cursors (main-screen and alt-screen). */
    int          saved_row,       saved_col;
    SolTermCell  saved_attrs;
    int          alt_saved_row,   alt_saved_col;
    SolTermCell  alt_saved_attrs;

    /* Cursor. */
    int          cur_row, cur_col;
    SolTermCell  cur_attrs;         /* current SGR pen */
    bool         cursor_visible;
    bool         pending_wrap;      /* set when cursor is past last col */

    /* Scroll region (0-based, inclusive). */
    int          margin_top;
    int          margin_bottom;

    /* Terminal modes. */
    bool         mode_cursor_app;   /* DECCKM: cursor keys send app sequences */
    bool         mode_keypad_app;   /* DECKPAM: keypad sends app sequences   */
    bool         mode_autowrap;     /* DECAWM: wrap at line end              */
    bool         mode_insert;       /* IRM: insert mode                      */
    bool         mode_origin;       /* DECOM: origin mode (cursor relative)  */
    bool         mode_linefeed;     /* LNM: automatic newline on LF          */
    bool         mode_bracketed_paste; /* XTerm bracketed paste              */

    /* View scroll offset (0 = show viewport, k = k scrollback lines above). */
    int          view_scroll;

    /* Window title. */
    char         title[SOL_TERM_TITLE_MAX];

    /* VT parser state. */
    SolVtState   vt_state;
    char         vt_intermediate[4];
    int          vt_n_intermediate;
    int          vt_params[SOL_TERM_MAX_PARAMS];
    int          vt_n_params;
    bool         vt_dcs_private;    /* '?' prefix in DEC mode sets */

    /* OSC accumulation. */
    char         osc_buf[SOL_TERM_OSC_MAX];
    int          osc_len;

    /* PTY file descriptor / process (platform-specific). */
#if defined(_WIN32)
    HPCON        hConsole;
    HANDLE       hProcess;
    HANDLE       hThread;
    HANDLE       hPipeIn;           /* we write here → child stdin  */
    HANDLE       hPipeOut;          /* child stdout → we read here  */
    /* ConPTY function pointers (dynamic). */
    HRESULT (*fn_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
    void    (*fn_ClosePseudoConsole)(HPCON);
    HRESULT (*fn_ResizePseudoConsole)(HPCON, COORD);
    HMODULE  hKernel;
#else
    int          master_fd;
    pid_t        child_pid;
#endif

    /* Reader thread. */
    pthread_t    reader_thread;
    bool         reader_started;
    atomic_bool  stop_reader;

    /* Lock-free ring buffer (reader thread writes, main reads). */
    pthread_mutex_t output_mutex;
    char            output_ring[SOL_TERM_OUTPUT_RING_SIZE];
    size_t          output_head;    /* next write position */
    size_t          output_tail;    /* next read position  */

    /* Lifecycle. */
    bool         is_alive;
    bool         dirty;             /* any cell changed since last drain */
};

/* ================================================================== */
/* SolTerminalManager                                                  */
/* ================================================================== */

struct SolTerminalManager {
    Ca_Instance  *instance;
    SolTerminal  *tabs[SOL_TERM_MAX_TABS];
    size_t        tab_count;
    size_t        active_index;

    bool                visible;
    bool                focused;
    SolTerminalPosition position;
    float               ratio;      /* fraction of the split for the terminal panel */
};

/* ================================================================== */
/* Color defaults                                                      */
/* ================================================================== */

/* ANSI 16-color palette matching the Sol retro-dark theme.
   Index 0-7: normal; 8-15: bright. */
static const uint32_t k_ansi16[16] = {
    0x1e1e24ff, /* 0  Black       */
    0xcc6666ff, /* 1  Red         */
    0xb5bd68ff, /* 2  Green       */
    0xde935fff, /* 3  Yellow      */
    0x81a2beff, /* 4  Blue        */
    0xb294bbff, /* 5  Magenta     */
    0x8abeb7ff, /* 6  Cyan        */
    0xc5c8c6ff, /* 7  White       */
    0x666666ff, /* 8  BrightBlack */
    0xcc6666ff, /* 9  BrightRed   */
    0xb5bd68ff, /* 10 BrightGreen */
    0xf0c674ff, /* 11 BrightYellow*/
    0x81a2beff, /* 12 BrightBlue  */
    0xb294bbff, /* 13 BrightMagenta */
    0x8abeb7ff, /* 14 BrightCyan  */
    0xffffffff, /* 15 BrightWhite */
};

/* Terminal default fg/bg colours (packed RGBA). */
#define SOL_TERM_DEFAULT_FG  0xc8c8ccff
#define SOL_TERM_DEFAULT_BG  0x0e0e10ff

/* ================================================================== */
/* Color helpers                                                       */
/* ================================================================== */

/*
 * Convert a SolTermColor to a packed RGBA uint32, used by the renderer.
 * is_fg: true to resolve default as DEFAULT_FG, false for DEFAULT_BG.
 */
uint32_t sol_term_color_to_rgba(const SolTermColor *c, bool is_fg)
{
    switch (c->mode) {
    case SOL_TERM_COLOR_DEFAULT:
        return is_fg ? SOL_TERM_DEFAULT_FG : SOL_TERM_DEFAULT_BG;
    case SOL_TERM_COLOR_INDEXED:
        if (c->index < 16) return k_ansi16[c->index];
        if (c->index < 232) {
            /* 6x6x6 colour cube: index 16-231. */
            uint8_t idx = c->index - 16;
            uint8_t b = idx % 6; idx /= 6;
            uint8_t g = idx % 6; idx /= 6;
            uint8_t r = idx % 6;
            return ((uint32_t)ansi_cube_to_byte(r) << 24) |
                   ((uint32_t)ansi_cube_to_byte(g) << 16) |
                   ((uint32_t)ansi_cube_to_byte(b) <<  8) | 0xffu;
        }
        /* Grayscale ramp: index 232-255. */
        { uint8_t v = 8 + (c->index - 232) * 10;
          return ((uint32_t)v << 24) | ((uint32_t)v << 16) |
                 ((uint32_t)v <<  8) | 0xffu; }
    case SOL_TERM_COLOR_RGB:
        return ((uint32_t)c->rgb.r << 24) |
               ((uint32_t)c->rgb.g << 16) |
               ((uint32_t)c->rgb.b <<  8) | 0xffu;
    default:
        return is_fg ? SOL_TERM_DEFAULT_FG : SOL_TERM_DEFAULT_BG;
    }
}

/* ================================================================== */
/* Cell grid helpers                                                   */
/* ================================================================== */

static SolTermCell blank_cell(void)
{
    SolTermCell c;
    memset(&c, 0, sizeof(c));
    c.fg.mode = SOL_TERM_COLOR_DEFAULT;
    c.bg.mode = SOL_TERM_COLOR_DEFAULT;
    return c;
}

/*
 * Allocate a SolTermLine of `cols` cells.
 * All cells are initialised to blank (space, default colors, no attributes).
 *
 * cols  Number of columns.
 * Returns heap-allocated line; cells are zeroed with default colors set.
 */
static bool term_line_alloc(SolTermLine *line, int cols)
{
    line->cells = (SolTermCell *)calloc((size_t)cols, sizeof(SolTermCell));
    if (!line->cells) return false;
    line->cols = cols;
    line->dirty = true;
    SolTermCell blank = blank_cell();
    for (int i = 0; i < cols; ++i) {
        line->cells[i] = blank;
    }
    return true;
}

static void term_line_free(SolTermLine *line)
{
    free(line->cells);
    line->cells = NULL;
    line->cols = 0;
}

/*
 * Erase cells [start, end) in `line` to blank (using the current pen attrs for bg).
 *
 * line   Target line.
 * start  First column to erase (inclusive).
 * end    One past the last column to erase.
 * attrs  Current SGR pen whose bg is used for the erased cells.
 */
static void term_line_erase(SolTermLine *line, int start, int end,
                            const SolTermCell *attrs)
{
    if (!line || !line->cells) return;
    if (start < 0) start = 0;
    if (end > line->cols) end = line->cols;
    SolTermCell blank = blank_cell();
    blank.bg = attrs->bg;
    for (int i = start; i < end; ++i) {
        line->cells[i] = blank;
    }
    line->dirty = true;
}

/* Copy `cols` cells from src to dst. */
static void term_line_copy(SolTermLine *dst, const SolTermLine *src, int cols)
{
    if (!dst->cells || !src->cells) return;
    int n = cols < dst->cols ? cols : dst->cols;
    if (n > src->cols) n = src->cols;
    memcpy(dst->cells, src->cells, (size_t)n * sizeof(SolTermCell));
    if (n < dst->cols) {
        SolTermCell blank = blank_cell();
        for (int i = n; i < dst->cols; ++i) dst->cells[i] = blank;
    }
    dst->dirty = true;
}

/* ================================================================== */
/* Terminal scrollback                                                 */
/* ================================================================== */

/*
 * Push the viewport's top line into the scrollback ring, then shift the
 * viewport up by one, erasing the bottom line.  Only called when
 * scrolling within the active scroll region.
 *
 * term   Terminal.
 * top    First row of the scroll region (0-based).
 * bot    Last row of the scroll region (0-based).
 * attrs  Current pen (bg used for new blank line).
 */
static void term_scroll_up_region(SolTerminal *term, int top, int bot,
                                  const SolTermCell *attrs)
{
    /* Push the exiting top line into scrollback only when scrolling the
       full screen (top==0 and bot==rows-1).  Region scrolls inside
       alternate screen or partial regions don't fill the scrollback. */
    if (top == 0 && bot == term->rows - 1 && !term->in_alt_screen) {
        int dest = (term->scrollback_head + term->scrollback_count) %
                   SOL_TERM_SCROLLBACK_MAX;
        if (term->scrollback_count < SOL_TERM_SCROLLBACK_MAX) {
            if (!term->scrollback[dest].cells) {
                term_line_alloc(&term->scrollback[dest], term->cols);
            }
            term_line_copy(&term->scrollback[dest], &term->screen[0], term->cols);
            term->scrollback_count++;
        } else {
            /* Ring is full: overwrite oldest (advance head). */
            term_line_copy(&term->scrollback[dest], &term->screen[0], term->cols);
            term->scrollback_head =
                (term->scrollback_head + 1) % SOL_TERM_SCROLLBACK_MAX;
        }
    }

    /* Shift viewport lines [top+1..bot] up by one. */
    for (int r = top; r < bot; ++r) {
        term_line_copy(&term->screen[r], &term->screen[r + 1], term->cols);
    }
    /* Clear the last line in the region. */
    term_line_erase(&term->screen[bot], 0, term->cols, attrs);
}

/*
 * Scroll the active scroll region down by one: shift lines [top..bot-1]
 * down, and clear the top line.
 *
 * term   Terminal.
 * top    First row of the region (0-based).
 * bot    Last row of the region (0-based).
 * attrs  Current pen (bg used for new blank line).
 */
static void term_scroll_down_region(SolTerminal *term, int top, int bot,
                                    const SolTermCell *attrs)
{
    for (int r = bot; r > top; --r) {
        term_line_copy(&term->screen[r], &term->screen[r - 1], term->cols);
    }
    term_line_erase(&term->screen[top], 0, term->cols, attrs);
}

/* ================================================================== */
/* VT — cursor movement helpers                                        */
/* ================================================================== */

static int term_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void term_move_cursor(SolTerminal *term, int row, int col)
{
    term->pending_wrap = false;
    term->cur_row = term_clamp(row, 0, term->rows - 1);
    term->cur_col = term_clamp(col, 0, term->cols - 1);
}

/*
 * Move to the next line, scrolling if the cursor is at the bottom of the
 * scroll region.
 *
 * term   Terminal.
 */
static void term_new_line(SolTerminal *term)
{
    term->pending_wrap = false;
    if (term->cur_row == term->margin_bottom) {
        term_scroll_up_region(term, term->margin_top, term->margin_bottom,
                              &term->cur_attrs);
    } else {
        term->cur_row = term_clamp(term->cur_row + 1, 0, term->rows - 1);
    }
}

/*
 * Write a codepoint into the current cursor cell and advance the cursor.
 *
 * term       Terminal.
 * codepoint  Unicode codepoint to write.
 */
static void term_put_char(SolTerminal *term, uint32_t codepoint)
{
    if (term->pending_wrap && term->mode_autowrap) {
        term->screen[term->cur_row].cells[term->cols - 1].attrs &=
            (uint16_t)~SOL_TERM_ATTR_WIDE;
        term_new_line(term);
        term->cur_col = 0;
    }
    term->pending_wrap = false;

    SolTermLine *line = &term->screen[term->cur_row];
    SolTermCell cell  = term->cur_attrs;
    cell.codepoint    = codepoint;

    if (term->mode_insert && term->cur_col < term->cols - 1) {
        memmove(&line->cells[term->cur_col + 1],
                &line->cells[term->cur_col],
                (size_t)(term->cols - term->cur_col - 1) * sizeof(SolTermCell));
    }

    line->cells[term->cur_col] = cell;
    line->dirty = true;
    term->dirty = true;

    if (term->cur_col >= term->cols - 1) {
        term->pending_wrap = true;
    } else {
        term->cur_col++;
    }
}

/* ================================================================== */
/* VT — C0 control execution                                           */
/* ================================================================== */

static void vt_execute(SolTerminal *term, uint8_t byte)
{
    switch (byte) {
    case 0x07: /* BEL — ignore */ break;
    case 0x08: /* BS */
        if (term->cur_col > 0) { term->cur_col--; term->pending_wrap = false; }
        break;
    case 0x09: /* HT — horizontal tab, advance to next tab stop (every 8 cols) */
        {
            int next = ((term->cur_col / 8) + 1) * 8;
            if (next >= term->cols) next = term->cols - 1;
            term->cur_col = next;
            term->pending_wrap = false;
        }
        break;
    case 0x0A: /* LF */
    case 0x0B: /* VT */
    case 0x0C: /* FF */
        term_new_line(term);
        if (term->mode_linefeed) term->cur_col = 0;
        break;
    case 0x0D: /* CR */
        term->cur_col = 0;
        term->pending_wrap = false;
        break;
    case 0x0E: /* SO — switch to G1 charset (ignore for now) */ break;
    case 0x0F: /* SI — switch to G0 charset (ignore for now) */ break;
    default: break;
    }
    term->dirty = true;
}

/* ================================================================== */
/* VT — SGR (Select Graphic Rendition) attribute handler               */
/* ================================================================== */

static void vt_sgr(SolTerminal *term)
{
    int n = term->vt_n_params;
    int *p = term->vt_params;

    if (n == 0) { p[0] = 0; n = 1; }

    for (int i = 0; i < n; ++i) {
        int v = p[i];
        switch (v) {
        case 0:
            memset(&term->cur_attrs, 0, sizeof(term->cur_attrs));
            term->cur_attrs.fg.mode = SOL_TERM_COLOR_DEFAULT;
            term->cur_attrs.bg.mode = SOL_TERM_COLOR_DEFAULT;
            break;
        case 1: term->cur_attrs.attrs |= SOL_TERM_ATTR_BOLD;      break;
        case 2: term->cur_attrs.attrs |= SOL_TERM_ATTR_DIM;       break;
        case 3: term->cur_attrs.attrs |= SOL_TERM_ATTR_ITALIC;    break;
        case 4: term->cur_attrs.attrs |= SOL_TERM_ATTR_UNDERLINE; break;
        case 5: term->cur_attrs.attrs |= SOL_TERM_ATTR_BLINK;     break;
        case 7: term->cur_attrs.attrs |= SOL_TERM_ATTR_REVERSE;   break;
        case 8: term->cur_attrs.attrs |= SOL_TERM_ATTR_INVISIBLE; break;
        case 9: term->cur_attrs.attrs |= SOL_TERM_ATTR_STRIKE;    break;
        case 22: term->cur_attrs.attrs &= (uint16_t)~(SOL_TERM_ATTR_BOLD | SOL_TERM_ATTR_DIM); break;
        case 23: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_ITALIC;    break;
        case 24: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_UNDERLINE; break;
        case 25: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_BLINK;     break;
        case 27: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_REVERSE;   break;
        case 28: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_INVISIBLE; break;
        case 29: term->cur_attrs.attrs &= (uint16_t)~SOL_TERM_ATTR_STRIKE;    break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            term->cur_attrs.fg.mode  = SOL_TERM_COLOR_INDEXED;
            term->cur_attrs.fg.index = (uint8_t)(v - 30);
            break;
        case 38:
            if (i + 2 < n && p[i+1] == 5) {
                term->cur_attrs.fg.mode  = SOL_TERM_COLOR_INDEXED;
                term->cur_attrs.fg.index = (uint8_t)p[i+2];
                i += 2;
            } else if (i + 4 < n && p[i+1] == 2) {
                term->cur_attrs.fg.mode    = SOL_TERM_COLOR_RGB;
                term->cur_attrs.fg.rgb.r   = (uint8_t)p[i+2];
                term->cur_attrs.fg.rgb.g   = (uint8_t)p[i+3];
                term->cur_attrs.fg.rgb.b   = (uint8_t)p[i+4];
                i += 4;
            }
            break;
        case 39:
            term->cur_attrs.fg.mode = SOL_TERM_COLOR_DEFAULT;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            term->cur_attrs.bg.mode  = SOL_TERM_COLOR_INDEXED;
            term->cur_attrs.bg.index = (uint8_t)(v - 40);
            break;
        case 48:
            if (i + 2 < n && p[i+1] == 5) {
                term->cur_attrs.bg.mode  = SOL_TERM_COLOR_INDEXED;
                term->cur_attrs.bg.index = (uint8_t)p[i+2];
                i += 2;
            } else if (i + 4 < n && p[i+1] == 2) {
                term->cur_attrs.bg.mode    = SOL_TERM_COLOR_RGB;
                term->cur_attrs.bg.rgb.r   = (uint8_t)p[i+2];
                term->cur_attrs.bg.rgb.g   = (uint8_t)p[i+3];
                term->cur_attrs.bg.rgb.b   = (uint8_t)p[i+4];
                i += 4;
            }
            break;
        case 49:
            term->cur_attrs.bg.mode = SOL_TERM_COLOR_DEFAULT;
            break;
        /* Bright fg: 90-97 */
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            term->cur_attrs.fg.mode  = SOL_TERM_COLOR_INDEXED;
            term->cur_attrs.fg.index = (uint8_t)(v - 90 + 8);
            break;
        /* Bright bg: 100-107 */
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            term->cur_attrs.bg.mode  = SOL_TERM_COLOR_INDEXED;
            term->cur_attrs.bg.index = (uint8_t)(v - 100 + 8);
            break;
        default: break;
        }
    }
}

/* ================================================================== */
/* VT — DEC private mode set/reset                                     */
/* ================================================================== */

static void vt_set_dec_mode(SolTerminal *term, int param, bool set)
{
    switch (param) {
    case 1:    /* DECCKM: application cursor keys */
        term->mode_cursor_app = set;
        break;
    case 7:    /* DECAWM: auto-wrap */
        term->mode_autowrap = set;
        break;
    case 12:   /* cursor blink — ignore */
        break;
    case 25:   /* DECTCEM: cursor visibility */
        term->cursor_visible = set;
        break;
    case 47:   /* alternate screen (simple, no cursor save) */
        if (set && !term->in_alt_screen) {
            /* allocate and switch to alt screen */
            if (!term->alt_screen) {
                term->alt_screen = (SolTermLine *)calloc(
                    (size_t)term->rows, sizeof(SolTermLine));
                if (term->alt_screen) {
                    term->alt_screen_rows = term->rows;
                    for (int r = 0; r < term->rows; ++r)
                        term_line_alloc(&term->alt_screen[r], term->cols);
                }
            }
            if (term->alt_screen) {
                term->in_alt_screen = true;
                /* Clear alt screen */
                for (int r = 0; r < term->rows; ++r) {
                    term_line_erase(&term->alt_screen[r], 0, term->cols,
                                    &term->cur_attrs);
                }
                /* Swap pointers */
                for (int r = 0; r < term->rows; ++r) {
                    SolTermLine tmp = term->screen[r];
                    term->screen[r] = term->alt_screen[r];
                    term->alt_screen[r] = tmp;
                }
            }
        } else if (!set && term->in_alt_screen) {
            if (term->alt_screen) {
                /* Swap back */
                for (int r = 0; r < term->rows; ++r) {
                    SolTermLine tmp = term->screen[r];
                    term->screen[r] = term->alt_screen[r];
                    term->alt_screen[r] = tmp;
                }
            }
            term->in_alt_screen = false;
        }
        break;
    case 1000: /* mouse button tracking — ignore */ break;
    case 1002: /* mouse any-event tracking — ignore */ break;
    case 1006: /* mouse SGR extended — ignore */ break;
    case 1049: /* alternate screen with cursor save/restore */
        if (set && !term->in_alt_screen) {
            /* Save main cursor */
            term->alt_saved_row   = term->cur_row;
            term->alt_saved_col   = term->cur_col;
            term->alt_saved_attrs = term->cur_attrs;
            vt_set_dec_mode(term, 47, true);
            /* Reset cursor in alt screen */
            term->cur_row = 0; term->cur_col = 0;
        } else if (!set && term->in_alt_screen) {
            vt_set_dec_mode(term, 47, false);
            /* Restore main cursor */
            term->cur_row   = term->alt_saved_row;
            term->cur_col   = term->alt_saved_col;
            term->cur_attrs = term->alt_saved_attrs;
        }
        break;
    case 2004: /* bracketed paste mode */
        term->mode_bracketed_paste = set;
        break;
    default: break;
    }
}

/* ================================================================== */
/* VT — CSI dispatch                                                   */
/* ================================================================== */

/* Resolve a param value, defaulting to `def` when the slot is 0 or unset. */
static int vt_param(const SolTerminal *term, int idx, int def)
{
    if (idx >= term->vt_n_params || term->vt_params[idx] == 0) return def;
    return term->vt_params[idx];
}

static void vt_csi_dispatch(SolTerminal *term, uint8_t final)
{
    const bool priv = term->vt_dcs_private;

    switch (final) {
    /* ---- Cursor movement ---- */
    case 'A': /* CUU: cursor up */
        term_move_cursor(term,
            term_clamp(term->cur_row - vt_param(term, 0, 1),
                       term->margin_top, term->margin_bottom),
            term->cur_col);
        break;
    case 'B': /* CUD: cursor down */
        term_move_cursor(term,
            term_clamp(term->cur_row + vt_param(term, 0, 1),
                       term->margin_top, term->margin_bottom),
            term->cur_col);
        break;
    case 'C': /* CUF: cursor forward */
        term_move_cursor(term, term->cur_row,
            term_clamp(term->cur_col + vt_param(term, 0, 1),
                       0, term->cols - 1));
        break;
    case 'D': /* CUB: cursor back */
        term_move_cursor(term, term->cur_row,
            term_clamp(term->cur_col - vt_param(term, 0, 1),
                       0, term->cols - 1));
        break;
    case 'E': /* CNL: cursor next line */
        term_move_cursor(term,
            term_clamp(term->cur_row + vt_param(term, 0, 1),
                       0, term->rows - 1), 0);
        break;
    case 'F': /* CPL: cursor previous line */
        term_move_cursor(term,
            term_clamp(term->cur_row - vt_param(term, 0, 1),
                       0, term->rows - 1), 0);
        break;
    case 'G': /* CHA: cursor horizontal absolute */
        term_move_cursor(term, term->cur_row,
            term_clamp(vt_param(term, 0, 1) - 1, 0, term->cols - 1));
        break;
    case 'H': /* CUP: cursor position */
    case 'f': /* HVP: horizontal and vertical position */
        term_move_cursor(term,
            term_clamp(vt_param(term, 0, 1) - 1, 0, term->rows - 1),
            term_clamp(vt_param(term, 1, 1) - 1, 0, term->cols - 1));
        break;
    case 'd': /* VPA: vertical position absolute */
        term_move_cursor(term,
            term_clamp(vt_param(term, 0, 1) - 1, 0, term->rows - 1),
            term->cur_col);
        break;

    /* ---- Erase ---- */
    case 'J': /* ED: erase in display */
        switch (vt_param(term, 0, 0)) {
        case 0: /* erase from cursor to end of screen */
            term_line_erase(&term->screen[term->cur_row],
                            term->cur_col, term->cols, &term->cur_attrs);
            for (int r = term->cur_row + 1; r < term->rows; ++r)
                term_line_erase(&term->screen[r], 0, term->cols, &term->cur_attrs);
            break;
        case 1: /* erase from start of screen to cursor */
            for (int r = 0; r < term->cur_row; ++r)
                term_line_erase(&term->screen[r], 0, term->cols, &term->cur_attrs);
            term_line_erase(&term->screen[term->cur_row],
                            0, term->cur_col + 1, &term->cur_attrs);
            break;
        case 2: /* erase all */
        case 3: /* erase all + scrollback (3 = xterm extension) */
            for (int r = 0; r < term->rows; ++r)
                term_line_erase(&term->screen[r], 0, term->cols, &term->cur_attrs);
            if (vt_param(term, 0, 0) == 3) {
                term->scrollback_count = 0;
                term->scrollback_head  = 0;
            }
            break;
        }
        break;
    case 'K': /* EL: erase in line */
        switch (vt_param(term, 0, 0)) {
        case 0: /* from cursor to end of line */
            term_line_erase(&term->screen[term->cur_row],
                            term->cur_col, term->cols, &term->cur_attrs);
            break;
        case 1: /* from start of line to cursor */
            term_line_erase(&term->screen[term->cur_row],
                            0, term->cur_col + 1, &term->cur_attrs);
            break;
        case 2: /* entire line */
            term_line_erase(&term->screen[term->cur_row],
                            0, term->cols, &term->cur_attrs);
            break;
        }
        break;
    case 'X': /* ECH: erase `n` characters from cursor */
        {
            int n = vt_param(term, 0, 1);
            term_line_erase(&term->screen[term->cur_row],
                            term->cur_col,
                            term_clamp(term->cur_col + n, 0, term->cols),
                            &term->cur_attrs);
        }
        break;

    /* ---- Insert / delete ---- */
    case 'L': /* IL: insert `n` lines */
        {
            int n = term_clamp(vt_param(term, 0, 1), 1, term->rows);
            for (int i = 0; i < n; ++i)
                term_scroll_down_region(term, term->cur_row, term->margin_bottom,
                                        &term->cur_attrs);
        }
        break;
    case 'M': /* DL: delete `n` lines */
        {
            int n = term_clamp(vt_param(term, 0, 1), 1, term->rows);
            for (int i = 0; i < n; ++i)
                term_scroll_up_region(term, term->cur_row, term->margin_bottom,
                                      &term->cur_attrs);
        }
        break;
    case 'P': /* DCH: delete `n` characters */
        {
            int n  = term_clamp(vt_param(term, 0, 1), 1, term->cols);
            int c  = term->cur_col;
            int rem = term->cols - c - n;
            SolTermLine *line = &term->screen[term->cur_row];
            if (rem > 0)
                memmove(&line->cells[c], &line->cells[c + n],
                        (size_t)rem * sizeof(SolTermCell));
            term_line_erase(line, term->cols - n, term->cols, &term->cur_attrs);
        }
        break;
    case '@': /* ICH: insert `n` blank characters */
        {
            int n   = term_clamp(vt_param(term, 0, 1), 1, term->cols);
            int c   = term->cur_col;
            int rem = term->cols - c - n;
            SolTermLine *line = &term->screen[term->cur_row];
            if (rem > 0)
                memmove(&line->cells[c + n], &line->cells[c],
                        (size_t)rem * sizeof(SolTermCell));
            term_line_erase(line, c, c + n, &term->cur_attrs);
        }
        break;

    /* ---- Scroll ---- */
    case 'S': /* SU: scroll up `n` lines */
        {
            int n = term_clamp(vt_param(term, 0, 1), 1, term->rows);
            for (int i = 0; i < n; ++i)
                term_scroll_up_region(term, term->margin_top, term->margin_bottom,
                                      &term->cur_attrs);
        }
        break;
    case 'T': /* SD: scroll down `n` lines */
        {
            int n = term_clamp(vt_param(term, 0, 1), 1, term->rows);
            for (int i = 0; i < n; ++i)
                term_scroll_down_region(term, term->margin_top, term->margin_bottom,
                                        &term->cur_attrs);
        }
        break;

    /* ---- Device attributes / status ---- */
    case 'c': /* DA: device attributes — report as VT102 */
        if (!priv || vt_param(term, 0, 0) == 0) {
#if !defined(_WIN32)
            const char *da = "\033[?6c";
            if (term->master_fd >= 0) {
                (void)write(term->master_fd, da, strlen(da));
            }
#endif
        }
        break;
    case 'n': /* DSR: device status report */
        if (vt_param(term, 0, 0) == 6) {
            /* CPR: cursor position report */
            char buf[32];
            snprintf(buf, sizeof(buf), "\033[%d;%dR",
                     term->cur_row + 1, term->cur_col + 1);
#if !defined(_WIN32)
            if (term->master_fd >= 0)
                (void)write(term->master_fd, buf, strlen(buf));
#endif
        }
        break;

    /* ---- Modes ---- */
    case 'h':
        if (priv) {
            for (int i = 0; i < term->vt_n_params; ++i)
                vt_set_dec_mode(term, term->vt_params[i], true);
        } else {
            /* Standard modes */
            if (vt_param(term, 0, 0) == 4) term->mode_insert = true;
            if (vt_param(term, 0, 0) == 20) term->mode_linefeed = true;
        }
        break;
    case 'l':
        if (priv) {
            for (int i = 0; i < term->vt_n_params; ++i)
                vt_set_dec_mode(term, term->vt_params[i], false);
        } else {
            if (vt_param(term, 0, 0) == 4) term->mode_insert = false;
            if (vt_param(term, 0, 0) == 20) term->mode_linefeed = false;
        }
        break;

    /* ---- SGR ---- */
    case 'm':
        vt_sgr(term);
        break;

    /* ---- Scroll region (DECSTBM) ---- */
    case 'r':
        if (!priv) {
            int top = term_clamp(vt_param(term, 0, 1) - 1, 0, term->rows - 2);
            int bot = term_clamp(vt_param(term, 1, term->rows) - 1,
                                 top + 1, term->rows - 1);
            term->margin_top    = top;
            term->margin_bottom = bot;
            term_move_cursor(term, 0, 0);
        }
        break;

    /* ---- Save / restore cursor ---- */
    case 's':
        term->saved_row   = term->cur_row;
        term->saved_col   = term->cur_col;
        term->saved_attrs = term->cur_attrs;
        break;
    case 'u':
        term->cur_row   = term->saved_row;
        term->cur_col   = term->saved_col;
        term->cur_attrs = term->saved_attrs;
        term->pending_wrap = false;
        break;

    default: break;
    }
    term->dirty = true;
}

/* ================================================================== */
/* VT — ESC dispatch                                                   */
/* ================================================================== */

static void vt_esc_dispatch(SolTerminal *term, uint8_t final)
{
    switch (final) {
    case '7': /* DECSC: save cursor */
        term->saved_row   = term->cur_row;
        term->saved_col   = term->cur_col;
        term->saved_attrs = term->cur_attrs;
        break;
    case '8': /* DECRC: restore cursor */
        term->cur_row   = term->saved_row;
        term->cur_col   = term->saved_col;
        term->cur_attrs = term->saved_attrs;
        term->pending_wrap = false;
        break;
    case 'c': /* RIS: reset to initial state */
        {
            int rows = term->rows, cols = term->cols;
            for (int r = 0; r < rows; ++r)
                term_line_erase(&term->screen[r], 0, cols, &term->cur_attrs);
            memset(&term->cur_attrs, 0, sizeof(term->cur_attrs));
            term->cur_attrs.fg.mode = SOL_TERM_COLOR_DEFAULT;
            term->cur_attrs.bg.mode = SOL_TERM_COLOR_DEFAULT;
            term->cur_row = 0; term->cur_col = 0;
            term->margin_top = 0; term->margin_bottom = rows - 1;
            term->mode_autowrap = true;
            term->mode_cursor_app = false;
            term->mode_insert = false;
            term->pending_wrap = false;
        }
        break;
    case 'D': /* IND: index (move down/scroll) */
        term_new_line(term);
        break;
    case 'E': /* NEL: next line */
        term_new_line(term);
        term->cur_col = 0;
        break;
    case 'M': /* RI: reverse index (scroll down if at top margin) */
        if (term->cur_row == term->margin_top) {
            term_scroll_down_region(term, term->margin_top, term->margin_bottom,
                                    &term->cur_attrs);
        } else if (term->cur_row > 0) {
            term->cur_row--;
        }
        break;
    case '>': /* DECKPNM: numeric keypad mode */
        term->mode_keypad_app = false;
        break;
    case '=': /* DECKPAM: application keypad mode */
        term->mode_keypad_app = true;
        break;
    default: break;
    }
    term->dirty = true;
}

/* ================================================================== */
/* VT — OSC dispatch                                                   */
/* ================================================================== */

static void vt_osc_dispatch(SolTerminal *term)
{
    /* Format: "Pn;text" where Pn is the OSC command number. */
    char *semi = (char *)memchr(term->osc_buf, ';', (size_t)term->osc_len);
    if (!semi) return;
    int cmd = (int)strtol(term->osc_buf, NULL, 10);
    const char *text = semi + 1;

    switch (cmd) {
    case 0: /* Set icon name and window title */
    case 1: /* Set icon name (treat same as title) */
    case 2: /* Set window title */
        {
            size_t len = (size_t)term->osc_len - (size_t)(text - term->osc_buf);
            if (len >= SOL_TERM_TITLE_MAX) len = SOL_TERM_TITLE_MAX - 1;
            memcpy(term->title, text, len);
            term->title[len] = '\0';
            term->dirty = true;
        }
        break;
    default: break;
    }
}

/* ================================================================== */
/* VT — main byte processor (Paul Williams state machine)              */
/* ================================================================== */

static void vt_process_byte(SolTerminal *term, uint8_t byte)
{
    /* Anywhere: handle C1 controls (0x9B = CSI, 0x9D = OSC, etc.) and
       ESC (0x1B) which transitions from any state. */
    if (byte == 0x1B) {
        term->vt_state        = VT_ESCAPE;
        term->vt_n_intermediate = 0;
        term->vt_n_params       = 0;
        term->vt_dcs_private    = false;
        return;
    }
    /* Cancel: CAN (0x18) or SUB (0x1A) return to GROUND. */
    if (byte == 0x18 || byte == 0x1A) {
        term->vt_state = VT_GROUND;
        return;
    }

    switch (term->vt_state) {
    /* ---- GROUND ---- */
    case VT_GROUND:
        if (byte < 0x20) {
            vt_execute(term, byte);
        } else if (byte == 0x7F) {
            /* DEL — ignore in ground state */
        } else if (byte >= 0x20) {
            /* Printable byte — decode UTF-8. For simplicity we treat each
               byte >= 0x20 as a Latin-1 character here; a full UTF-8
               accumulator would be needed for non-BMP codepoints. The
               reader loop below handles multi-byte sequences properly.
               This branch is only reached for ASCII and valid single-byte
               characters. */
            term_put_char(term, (uint32_t)byte);
        }
        break;

    /* ---- ESCAPE ---- */
    case VT_ESCAPE:
        if (byte >= 0x20 && byte <= 0x2F) {
            /* Intermediate */
            if (term->vt_n_intermediate < 4)
                term->vt_intermediate[term->vt_n_intermediate++] = (char)byte;
            term->vt_state = VT_ESCAPE_INT;
        } else if (byte == '[') {
            term->vt_state       = VT_CSI_ENTRY;
            term->vt_n_params    = 0;
            term->vt_n_intermediate = 0;
            term->vt_dcs_private = false;
            memset(term->vt_params, 0, sizeof(term->vt_params));
        } else if (byte == ']') {
            term->vt_state  = VT_OSC_STRING;
            term->osc_len   = 0;
        } else if (byte == 'P') {
            term->vt_state = VT_DCS_ENTRY;
        } else if (byte == 'X' || byte == '^' || byte == '_') {
            term->vt_state = VT_SOS_PM_APC;
        } else if (byte >= 0x30 && byte <= 0x7E) {
            /* Final byte: dispatch escape sequence */
            vt_esc_dispatch(term, byte);
            term->vt_state = VT_GROUND;
        } else if (byte < 0x20) {
            vt_execute(term, byte);
        } else {
            term->vt_state = VT_GROUND;
        }
        break;

    /* ---- ESCAPE_INT ---- */
    case VT_ESCAPE_INT:
        if (byte >= 0x20 && byte <= 0x2F) {
            if (term->vt_n_intermediate < 4)
                term->vt_intermediate[term->vt_n_intermediate++] = (char)byte;
        } else if (byte >= 0x30 && byte <= 0x7E) {
            vt_esc_dispatch(term, byte);
            term->vt_state = VT_GROUND;
        } else if (byte < 0x20) {
            vt_execute(term, byte);
        } else {
            term->vt_state = VT_GROUND;
        }
        break;

    /* ---- CSI_ENTRY ---- */
    case VT_CSI_ENTRY:
        term->vt_n_params = 0;
        memset(term->vt_params, 0, sizeof(term->vt_params));
        term->vt_n_intermediate = 0;
        term->vt_dcs_private    = false;
        term->vt_state          = VT_CSI_PARAM;
        /* Fall through to process `byte` in CSI_PARAM. */
        /* FALLTHROUGH */
    case VT_CSI_PARAM:
        if (byte == '?') {
            term->vt_dcs_private = true;
        } else if (byte >= '0' && byte <= '9') {
            if (term->vt_n_params == 0) term->vt_n_params = 1;
            int *last = &term->vt_params[term->vt_n_params - 1];
            *last = *last * 10 + (byte - '0');
        } else if (byte == ';') {
            if (term->vt_n_params < SOL_TERM_MAX_PARAMS) term->vt_n_params++;
        } else if (byte >= 0x40 && byte <= 0x7E) {
            if (term->vt_n_params == 0) term->vt_n_params = 1;
            vt_csi_dispatch(term, byte);
            term->vt_state = VT_GROUND;
        } else if (byte >= 0x20 && byte <= 0x2F) {
            term->vt_state = VT_CSI_INT;
        } else if (byte == 0x3C || byte == 0x3D || byte == 0x3E || byte == 0x3F) {
            /* Private parameter prefix already handled above (?); others ignored */
        } else if (byte < 0x20) {
            vt_execute(term, byte);
        }
        break;

    /* ---- CSI_INT ---- */
    case VT_CSI_INT:
        if (byte >= 0x20 && byte <= 0x2F) {
            /* collect */
        } else if (byte >= 0x40 && byte <= 0x7E) {
            vt_csi_dispatch(term, byte);
            term->vt_state = VT_GROUND;
        } else if (byte < 0x20) {
            vt_execute(term, byte);
        } else {
            term->vt_state = VT_CSI_IGNORE;
        }
        break;

    /* ---- CSI_IGNORE ---- */
    case VT_CSI_IGNORE:
        if (byte >= 0x40 && byte <= 0x7E)
            term->vt_state = VT_GROUND;
        else if (byte < 0x20)
            vt_execute(term, byte);
        break;

    /* ---- DCS states (passthrough, ignore content) ---- */
    case VT_DCS_ENTRY:
    case VT_DCS_PARAM:
    case VT_DCS_INT:
    case VT_DCS_PASSTHROUGH:
    case VT_DCS_IGNORE:
        if (byte == 0x9C || byte == 0x1B) {
            /* ST or ESC terminates DCS */
            if (byte == 0x1B) {
                /* ESC starts escape state, will transition on next byte */
                term->vt_state = VT_ESCAPE;
                term->vt_n_intermediate = 0;
            } else {
                term->vt_state = VT_GROUND;
            }
        }
        break;

    /* ---- OSC_STRING ---- */
    case VT_OSC_STRING:
        if (byte == 0x07 || byte == 0x9C) {
            /* BEL or ST terminates OSC */
            vt_osc_dispatch(term);
            term->vt_state = VT_GROUND;
        } else if (byte == 0x1B) {
            /* ESC could be start of ESC \ (ST) — handle next byte */
            term->vt_state = VT_ESCAPE;
            term->vt_n_intermediate = 0;
            /* Flush OSC before transitioning: ST = ESC + '\' */
            /* The '\' dispatch in ESCAPE will be a no-op; OSC is already done. */
            vt_osc_dispatch(term);
        } else if (byte >= 0x20) {
            if (term->osc_len < SOL_TERM_OSC_MAX - 1) {
                term->osc_buf[term->osc_len++] = (char)byte;
            }
        }
        break;

    /* ---- SOS/PM/APC: ignore until ST ---- */
    case VT_SOS_PM_APC:
        if (byte == 0x9C)
            term->vt_state = VT_GROUND;
        break;
    }
}

/* UTF-8 multi-byte accumulation state for the VT processor.
   Bytes that form a multi-byte sequence are assembled here before
   calling term_put_char with the full codepoint. */
typedef struct VtUtf8 {
    uint8_t buf[4];
    int     remaining;
    uint32_t codepoint;
} VtUtf8;

static void vt_utf8_reset(VtUtf8 *u)
{
    u->remaining = 0;
    u->codepoint = 0;
    memset(u->buf, 0, sizeof(u->buf));
}

/*
 * Feed one byte through the UTF-8 decoder. When a complete codepoint is
 * assembled, write it to the terminal via term_put_char and reset the
 * accumulator. Falls back to Latin-1 on invalid sequences.
 *
 * term  Terminal.
 * u     UTF-8 accumulator state.
 * byte  Incoming byte.
 */
static void vt_utf8_feed(SolTerminal *term, VtUtf8 *u, uint8_t byte)
{
    if (u->remaining > 0) {
        if ((byte & 0xC0u) == 0x80u) {
            u->codepoint = (u->codepoint << 6) | (byte & 0x3Fu);
            u->remaining--;
            if (u->remaining == 0) {
                term_put_char(term, u->codepoint);
                vt_utf8_reset(u);
            }
            return;
        }
        /* Invalid continuation — discard accumulator, fall through. */
        vt_utf8_reset(u);
    }

    if ((byte & 0x80u) == 0x00u) {
        /* ASCII 0x00-0x7F: handled by vt_process_byte for controls,
           or directly put for printable. */
        vt_process_byte(term, byte);
    } else if ((byte & 0xE0u) == 0xC0u) {
        u->codepoint  = byte & 0x1Fu;
        u->remaining  = 1;
    } else if ((byte & 0xF0u) == 0xE0u) {
        u->codepoint  = byte & 0x0Fu;
        u->remaining  = 2;
    } else if ((byte & 0xF8u) == 0xF0u) {
        u->codepoint  = byte & 0x07u;
        u->remaining  = 3;
    } else {
        /* Invalid lead byte — treat as Latin-1. */
        term_put_char(term, (uint32_t)byte);
    }
}

/* ================================================================== */
/* PTY backend — Unix                                                   */
/* ================================================================== */

#if !defined(_WIN32)

/*
 * Reader thread: blocks on read() from the PTY master fd, deposits bytes
 * into the ring buffer, then wakes the Causality instance.
 *
 * arg  SolTerminal pointer.
 * Returns NULL always.
 */
static void *sol_terminal_reader_thread(void *arg)
{
    SolTerminal *term = (SolTerminal *)arg;
    char buf[32768];
    /* Capture fd locally so struct field changes (set to -1 during stop)
       don't affect the blocking read mid-call. */
    const int fd = term->master_fd;

    while (!atomic_load_explicit(&term->stop_reader, memory_order_relaxed)) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break; /* EIO (slave closed), EBADF (fd closed), or EOF */
        }

        pthread_mutex_lock(&term->output_mutex);
        for (ssize_t i = 0; i < n; ++i) {
            size_t next = (term->output_head + 1) % SOL_TERM_OUTPUT_RING_SIZE;
            if (next != term->output_tail) {
                term->output_ring[term->output_head] = buf[i];
                term->output_head = next;
            }
            /* Drop bytes if ring is full rather than blocking the reader. */
        }
        pthread_mutex_unlock(&term->output_mutex);

        ca_instance_wake();
    }

    term->is_alive = false;
    ca_instance_wake();
    return NULL;
}

/*
 * Launch a PTY-backed shell process. Sets master_fd, child_pid, and starts
 * the reader thread.
 *
 * term  Terminal to initialise.
 * Returns true on success.
 */
static bool sol_terminal_start_pty(SolTerminal *term)
{
    struct winsize ws = { 0 };
    ws.ws_col = (unsigned short)term->cols;
    ws.ws_row = (unsigned short)term->rows;

    const char *shell = getenv("SHELL");
    if (!shell || shell[0] == '\0') shell = "/bin/sh";

    pid_t pid = forkpty(&term->master_fd, NULL, NULL, &ws);
    if (pid < 0) return false;

    if (pid == 0) {
        /* Child: exec the shell. */
        const char *argv[] = { shell, NULL };
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execvp(shell, (char *const *)argv);
        _exit(127);
    }

    term->child_pid = pid;

    /* Set master fd to non-blocking to avoid reader stalls. */
    int flags = fcntl(term->master_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(term->master_fd, F_SETFL, flags | O_NONBLOCK);

    /* Re-enable blocking for the reader thread via select-based wait.
       Actually, keep non-blocking and use blocking read by clearing O_NONBLOCK
       in the reader thread — simpler: just use blocking reads (remove flag). */
    if (flags >= 0)
        fcntl(term->master_fd, F_SETFL, flags & ~O_NONBLOCK);

    atomic_store_explicit(&term->stop_reader, false, memory_order_relaxed);
    if (pthread_create(&term->reader_thread, NULL,
                       sol_terminal_reader_thread, term) != 0) {
        close(term->master_fd);
        term->master_fd = -1;
        kill(pid, SIGKILL);
        return false;
    }
    term->reader_started = true;
    return true;
}

static void sol_terminal_stop_pty(SolTerminal *term)
{
    atomic_store_explicit(&term->stop_reader, true, memory_order_relaxed);

    /* Kill child first so the slave PTY side closes, which makes the
       blocking read() on the master return with EIO.  If we close the
       master fd first instead, the read() may not return on macOS. */
    if (term->child_pid > 0) {
        kill(term->child_pid, SIGKILL);
    }

    /* Close master fd after the child is killed. */
    if (term->master_fd >= 0) {
        int fd = term->master_fd;
        term->master_fd = -1;   /* clear before close so reader sees -1 */
        close(fd);
    }

    /* Now safe to join — the read() will have returned. */
    if (term->reader_started) {
        pthread_join(term->reader_thread, NULL);
        term->reader_started = false;
    }

    /* Reap the child; WNOHANG first in case the drain already reaped it. */
    if (term->child_pid > 0) {
        int status;
        if (waitpid(term->child_pid, &status, WNOHANG) == 0) {
            /* Child not yet reaped; wait with timeout via blocking call.
               Should return immediately since we sent SIGKILL above. */
            waitpid(term->child_pid, &status, 0);
        }
        term->child_pid = 0;
    }
}

#else /* _WIN32 */

/* ================================================================== */
/* PTY backend — Windows ConPTY                                        */
/* ================================================================== */

static void *sol_terminal_reader_thread(void *arg)
{
    SolTerminal *term = (SolTerminal *)arg;
    char buf[4096];
    DWORD n;

    while (!atomic_load_explicit(&term->stop_reader, memory_order_relaxed)) {
        BOOL ok = ReadFile(term->hPipeOut, buf, (DWORD)sizeof(buf), &n, NULL);
        if (!ok || n == 0) break;

        pthread_mutex_lock(&term->output_mutex);
        for (DWORD i = 0; i < n; ++i) {
            size_t next = (term->output_head + 1) % SOL_TERM_OUTPUT_RING_SIZE;
            if (next != term->output_tail) {
                term->output_ring[term->output_head] = buf[i];
                term->output_head = next;
            }
        }
        pthread_mutex_unlock(&term->output_mutex);
        ca_instance_wake();
    }

    term->is_alive = false;
    ca_instance_wake();
    return NULL;
}

static bool sol_terminal_start_pty(SolTerminal *term)
{
    HMODULE hKernel = LoadLibraryA("kernel32.dll");
    if (!hKernel) return false;
    term->hKernel = hKernel;

    term->fn_CreatePseudoConsole =
        (HRESULT(*)(COORD,HANDLE,HANDLE,DWORD,HPCON*))
        GetProcAddress(hKernel, "CreatePseudoConsole");
    term->fn_ClosePseudoConsole =
        (void(*)(HPCON))GetProcAddress(hKernel, "ClosePseudoConsole");
    term->fn_ResizePseudoConsole =
        (HRESULT(*)(HPCON,COORD))GetProcAddress(hKernel, "ResizePseudoConsole");

    if (!term->fn_CreatePseudoConsole || !term->fn_ClosePseudoConsole ||
        !term->fn_ResizePseudoConsole) {
        FreeLibrary(hKernel);
        term->hKernel = NULL;
        return false;
    }

    /* Create pipe pair: we write to hPipeIn (child reads), child writes to
       hPipeOut pipe (we read). */
    HANDLE hChildIn_Rd, hChildIn_Wr;
    HANDLE hChildOut_Rd, hChildOut_Wr;
    if (!CreatePipe(&hChildIn_Rd, &hChildIn_Wr, NULL, 0)) return false;
    if (!CreatePipe(&hChildOut_Rd, &hChildOut_Wr, NULL, 0)) {
        CloseHandle(hChildIn_Rd); CloseHandle(hChildIn_Wr);
        return false;
    }

    COORD size = { (SHORT)term->cols, (SHORT)term->rows };
    HPCON hConsole = NULL;
    HRESULT hr = term->fn_CreatePseudoConsole(
        size, hChildIn_Rd, hChildOut_Wr, 0, &hConsole);

    /* ConPTY owns these ends now. */
    CloseHandle(hChildIn_Rd);
    CloseHandle(hChildOut_Wr);

    if (FAILED(hr)) {
        CloseHandle(hChildIn_Wr); CloseHandle(hChildOut_Rd);
        return false;
    }

    term->hConsole  = hConsole;
    term->hPipeIn   = hChildIn_Wr;
    term->hPipeOut  = hChildOut_Rd;

    /* Build STARTUPINFOEX with the ConPTY attribute. */
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST pAttrList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    if (!pAttrList) goto fail;
    if (!InitializeProcThreadAttributeList(pAttrList, 1, 0, &attrSize)) goto fail;
    if (!UpdateProcThreadAttribute(pAttrList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hConsole,
            sizeof(HPCON), NULL, NULL)) goto fail;

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb    = sizeof(si);
    si.lpAttributeList   = pAttrList;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* Use cmd.exe as the default shell on Windows. */
    wchar_t cmdline[] = L"cmd.exe";
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        DeleteProcThreadAttributeList(pAttrList);
        HeapFree(GetProcessHeap(), 0, pAttrList);
        goto fail;
    }

    DeleteProcThreadAttributeList(pAttrList);
    HeapFree(GetProcessHeap(), 0, pAttrList);

    term->hProcess = pi.hProcess;
    term->hThread  = pi.hThread;

    atomic_store_explicit(&term->stop_reader, false, memory_order_relaxed);
    if (pthread_create(&term->reader_thread, NULL,
                       sol_terminal_reader_thread, term) != 0) {
        goto fail;
    }
    term->reader_started = true;
    return true;

fail:
    if (term->fn_ClosePseudoConsole) term->fn_ClosePseudoConsole(term->hConsole);
    CloseHandle(term->hPipeIn);  CloseHandle(term->hPipeOut);
    if (term->hKernel) { FreeLibrary(term->hKernel); term->hKernel = NULL; }
    return false;
}

static void sol_terminal_stop_pty(SolTerminal *term)
{
    atomic_store_explicit(&term->stop_reader, true, memory_order_relaxed);
    if (term->hPipeIn  != NULL) { CloseHandle(term->hPipeIn);  term->hPipeIn  = NULL; }
    if (term->hPipeOut != NULL) { CloseHandle(term->hPipeOut); term->hPipeOut = NULL; }
    if (term->reader_started) {
        pthread_join(term->reader_thread, NULL);
        term->reader_started = false;
    }
    if (term->hProcess) { TerminateProcess(term->hProcess, 0); CloseHandle(term->hProcess); term->hProcess = NULL; }
    if (term->hThread)  { CloseHandle(term->hThread);          term->hThread  = NULL; }
    if (term->hConsole && term->fn_ClosePseudoConsole) {
        term->fn_ClosePseudoConsole(term->hConsole);
        term->hConsole = NULL;
    }
    if (term->hKernel) { FreeLibrary(term->hKernel); term->hKernel = NULL; }
}

#endif /* _WIN32 */

/* ================================================================== */
/* Terminal lifecycle                                                   */
/* ================================================================== */

/*
 * Create and launch a new terminal session.
 *
 * instance  Causality instance for wake signals.
 * Returns   Heap-allocated terminal, or NULL on failure.
 */
static SolTerminal *sol_terminal_create(Ca_Instance *instance)
{
    SolTerminal *term = (SolTerminal *)calloc(1u, sizeof(SolTerminal));
    if (!term) return NULL;

    term->instance     = instance;
    term->rows         = SOL_TERM_DEFAULT_ROWS;
    term->cols         = SOL_TERM_DEFAULT_COLS;
    term->margin_top   = 0;
    term->margin_bottom = SOL_TERM_DEFAULT_ROWS - 1;
    term->mode_autowrap = true;
    term->cursor_visible = true;
    term->is_alive      = true;

    term->cur_attrs.fg.mode = SOL_TERM_COLOR_DEFAULT;
    term->cur_attrs.bg.mode = SOL_TERM_COLOR_DEFAULT;

    snprintf(term->title, sizeof(term->title), "Terminal");

#if !defined(_WIN32)
    term->master_fd = -1;
    term->child_pid = 0;
#endif

    pthread_mutex_init(&term->output_mutex, NULL);

    /* Allocate screen lines. */
    for (int r = 0; r < term->rows; ++r) {
        if (!term_line_alloc(&term->screen[r], term->cols)) {
            /* Cleanup on partial allocation. */
            for (int i = 0; i < r; ++i) term_line_free(&term->screen[i]);
            free(term);
            return NULL;
        }
    }

    if (!sol_terminal_start_pty(term)) {
        for (int r = 0; r < term->rows; ++r) term_line_free(&term->screen[r]);
        pthread_mutex_destroy(&term->output_mutex);
        free(term);
        return NULL;
    }

    return term;
}

/*
 * Destroy a terminal session, stopping the PTY and freeing all memory.
 *
 * term  Terminal to destroy (safe to call with NULL).
 */
static void sol_terminal_destroy(SolTerminal *term)
{
    if (!term) return;
    sol_terminal_stop_pty(term);
    for (int r = 0; r < SOL_TERM_DEFAULT_ROWS + 64; ++r)
        term_line_free(&term->screen[r]);
    for (int r = 0; r < SOL_TERM_SCROLLBACK_MAX; ++r)
        term_line_free(&term->scrollback[r]);
    if (term->alt_screen) {
        for (int r = 0; r < term->alt_screen_rows; ++r)
            term_line_free(&term->alt_screen[r]);
        free(term->alt_screen);
    }
    pthread_mutex_destroy(&term->output_mutex);
    free(term);
}

/* ================================================================== */
/* Public API — manager                                                */
/* ================================================================== */

SolTerminalManager *sol_terminal_manager_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    SolTerminalManager *mgr =
        (SolTerminalManager *)calloc(1u, sizeof(SolTerminalManager));
    if (!mgr) return NULL;
    mgr->instance = instance;
    mgr->ratio    = 0.30f;
    mgr->position = SOL_TERMINAL_POSITION_BOTTOM;
    return mgr;
}

void sol_terminal_manager_destroy(SolTerminalManager *mgr)
{
    if (!mgr) return;
    for (size_t i = 0; i < mgr->tab_count; ++i)
        sol_terminal_destroy(mgr->tabs[i]);
    free(mgr);
}

SolTerminal *sol_terminal_manager_new_tab(SolTerminalManager *mgr)
{
    if (!mgr || mgr->tab_count >= SOL_TERM_MAX_TABS) return NULL;
    SolTerminal *term = sol_terminal_create(mgr->instance);
    if (!term) return NULL;
    mgr->tabs[mgr->tab_count++] = term;
    mgr->active_index = mgr->tab_count - 1;
    return term;
}

void sol_terminal_manager_close_active(SolTerminalManager *mgr)
{
    if (!mgr || mgr->tab_count == 0) return;
    size_t idx = mgr->active_index;
    sol_terminal_destroy(mgr->tabs[idx]);
    /* Shift remaining tabs down. */
    for (size_t i = idx; i + 1 < mgr->tab_count; ++i)
        mgr->tabs[i] = mgr->tabs[i + 1];
    mgr->tab_count--;
    mgr->tabs[mgr->tab_count] = NULL;
    if (mgr->tab_count == 0) {
        mgr->active_index = 0;
        mgr->focused = false;
        mgr->visible = false;
    } else {
        if (mgr->active_index >= mgr->tab_count)
            mgr->active_index = mgr->tab_count - 1;
    }
}

void sol_terminal_manager_next_tab(SolTerminalManager *mgr)
{
    if (!mgr || mgr->tab_count == 0) return;
    mgr->active_index = (mgr->active_index + 1) % mgr->tab_count;
}

void sol_terminal_manager_prev_tab(SolTerminalManager *mgr)
{
    if (!mgr || mgr->tab_count == 0) return;
    mgr->active_index =
        (mgr->active_index + mgr->tab_count - 1) % mgr->tab_count;
}

SolTerminal *sol_terminal_manager_active(const SolTerminalManager *mgr)
{
    if (!mgr || mgr->tab_count == 0) return NULL;
    return mgr->tabs[mgr->active_index];
}

size_t sol_terminal_manager_count(const SolTerminalManager *mgr)
{
    return mgr ? mgr->tab_count : 0u;
}

size_t sol_terminal_manager_active_index(const SolTerminalManager *mgr)
{
    return mgr ? mgr->active_index : 0u;
}

SolTerminal *sol_terminal_manager_at(const SolTerminalManager *mgr, size_t index)
{
    if (!mgr || index >= mgr->tab_count) return NULL;
    return mgr->tabs[index];
}

bool sol_terminal_manager_visible(const SolTerminalManager *mgr)
{
    return mgr ? mgr->visible : false;
}

void sol_terminal_manager_set_visible(SolTerminalManager *mgr, bool visible)
{
    if (mgr) mgr->visible = visible;
}

bool sol_terminal_manager_focused(const SolTerminalManager *mgr)
{
    return mgr ? mgr->focused : false;
}

void sol_terminal_manager_set_focused(SolTerminalManager *mgr, bool focused)
{
    if (!mgr) return;
    mgr->focused = focused;
    /* Ensure terminal is visible when focused. */
    if (focused && !mgr->visible) mgr->visible = true;
}

SolTerminalPosition sol_terminal_manager_position(const SolTerminalManager *mgr)
{
    return mgr ? mgr->position : SOL_TERMINAL_POSITION_BOTTOM;
}

void sol_terminal_manager_set_position(SolTerminalManager *mgr,
                                       SolTerminalPosition pos)
{
    if (mgr) mgr->position = pos;
}

float sol_terminal_manager_ratio(const SolTerminalManager *mgr)
{
    return mgr ? mgr->ratio : 0.30f;
}

void sol_terminal_manager_set_ratio(SolTerminalManager *mgr, float ratio)
{
    if (!mgr) return;
    mgr->ratio = ratio < 0.10f ? 0.10f : (ratio > 0.80f ? 0.80f : ratio);
}

/* ================================================================== */
/* Public API — drain                                                  */
/* ================================================================== */

bool sol_terminal_manager_drain(SolTerminalManager *mgr)
{
    if (!mgr) return false;
    bool any_dirty = false;
    for (size_t i = 0; i < mgr->tab_count; ++i) {
        SolTerminal *term = mgr->tabs[i];
        if (!term) continue;

        /* Reap dead children (Unix). */
#if !defined(_WIN32)
        if (term->is_alive && term->child_pid > 0) {
            int status;
            if (waitpid(term->child_pid, &status, WNOHANG) > 0) {
                term->is_alive = false;
                term->child_pid = 0;    /* prevent double-reap in stop_pty */
            }
        }
#endif

        /* Drain ring buffer into VT parser — consume all available bytes in
           one call so fast-output programs like cmatrix never accumulate a
           multi-frame backlog that causes perceptible lag or ring overflow. */
        char local[SOL_TERM_OUTPUT_RING_SIZE];
        size_t n = 0;
        pthread_mutex_lock(&term->output_mutex);
        while (term->output_tail != term->output_head) {
            local[n++] = term->output_ring[term->output_tail];
            term->output_tail = (term->output_tail + 1) % SOL_TERM_OUTPUT_RING_SIZE;
        }
        pthread_mutex_unlock(&term->output_mutex);

        if (n > 0) {
            term->dirty = false;
            VtUtf8 u; vt_utf8_reset(&u);
            for (size_t j = 0; j < n; ++j)
                vt_utf8_feed(term, &u, (uint8_t)local[j]);
            any_dirty = true;
        }
    }
    return any_dirty;
}

/* ================================================================== */
/* Public API — per-terminal                                           */
/* ================================================================== */

void sol_terminal_resize(SolTerminal *term, int cols, int rows)
{
    if (!term || cols < 1 || rows < 1) return;
    if (cols == term->cols && rows == term->rows) return;

    /* Clamp to grid capacity. */
    if (rows > SOL_TERM_DEFAULT_ROWS + 64) rows = SOL_TERM_DEFAULT_ROWS + 64;

    /* Resize screen lines. */
    for (int r = 0; r < rows; ++r) {
        if (r >= term->rows) {
            /* New rows beyond old rows: allocate. */
            term_line_alloc(&term->screen[r], cols);
        } else if (cols != term->cols) {
            /* Existing rows: realloc. */
            SolTermCell *newcells =
                (SolTermCell *)realloc(term->screen[r].cells,
                                       (size_t)cols * sizeof(SolTermCell));
            if (!newcells) continue;
            if (cols > term->cols) {
                SolTermCell blank = blank_cell();
                for (int c = term->cols; c < cols; ++c)
                    newcells[c] = blank;
            }
            term->screen[r].cells = newcells;
            term->screen[r].cols  = cols;
            term->screen[r].dirty = true;
        }
    }
    /* Free rows that no longer exist. */
    for (int r = rows; r < term->rows; ++r)
        term_line_free(&term->screen[r]);

    /* Sync alt_screen allocation to match the new dimensions. */
    if (term->alt_screen) {
        if (rows > term->alt_screen_rows) {
            /* Grow: reallocate the array, zero-init new entries, alloc their cells. */
            SolTermLine *grown = (SolTermLine *)realloc(
                term->alt_screen, (size_t)rows * sizeof(SolTermLine));
            if (grown) {
                term->alt_screen = grown;
                for (int r = term->alt_screen_rows; r < rows; ++r) {
                    memset(&term->alt_screen[r], 0, sizeof(SolTermLine));
                    term_line_alloc(&term->alt_screen[r], cols);
                }
                term->alt_screen_rows = rows;
            }
            /* On alloc failure, keep old size — safer to clip than to corrupt. */
        }
        /* Resize columns for all allocated alt_screen rows. */
        int resize_rows = (rows < term->alt_screen_rows) ? rows : term->alt_screen_rows;
        for (int r = 0; r < resize_rows; ++r) {
            if (cols == term->cols) continue;
            SolTermCell *newcells = (SolTermCell *)realloc(
                term->alt_screen[r].cells, (size_t)cols * sizeof(SolTermCell));
            if (!newcells) continue;
            if (cols > term->cols) {
                SolTermCell blank = blank_cell();
                for (int c = term->cols; c < cols; ++c)
                    newcells[c] = blank;
            }
            term->alt_screen[r].cells = newcells;
            term->alt_screen[r].cols  = cols;
        }
        /* Shrink: rows beyond new count are no longer needed. */
        if (rows < term->alt_screen_rows) {
            for (int r = rows; r < term->alt_screen_rows; ++r)
                term_line_free(&term->alt_screen[r]);
            term->alt_screen_rows = rows;
        }
    }

    term->cols = cols;
    term->rows = rows;
    term->margin_top    = 0;
    term->margin_bottom = rows - 1;
    term->cur_row = term_clamp(term->cur_row, 0, rows - 1);
    term->cur_col = term_clamp(term->cur_col, 0, cols - 1);

    /* Notify PTY. */
#if !defined(_WIN32)
    if (term->master_fd >= 0) {
        struct winsize ws = { 0 };
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ioctl(term->master_fd, TIOCSWINSZ, &ws);
    }
#else
    if (term->hConsole && term->fn_ResizePseudoConsole) {
        COORD size = { (SHORT)cols, (SHORT)rows };
        term->fn_ResizePseudoConsole(term->hConsole, size);
    }
#endif
}

void sol_terminal_send_text(SolTerminal *term, const char *data, size_t len)
{
    if (!term || !data || len == 0 || !term->is_alive) return;
#if !defined(_WIN32)
    if (term->master_fd < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(term->master_fd, data + off, len - off);
        if (n <= 0) { if (errno == EINTR) continue; break; }
        off += (size_t)n;
    }
#else
    DWORD written;
    WriteFile(term->hPipeIn, data, (DWORD)len, &written, NULL);
#endif
}

/* Modifier bitmask for xterm-style modified key sequences (1-based). */
static int term_modifier_number(uint8_t mods)
{
    /* modifier = 1 + shift + (alt<<1) + (ctrl<<2) */
    int m = 1;
    if (mods & SOL_MOD_SHIFT) m |= 1;
    if (mods & SOL_MOD_ALT)   m |= 2;
    if (mods & SOL_MOD_CTRL)  m |= 4;
    return m;
}

void sol_terminal_send_key(SolTerminal *term, uint32_t key, uint8_t mods)
{
    if (!term || !term->is_alive) return;

    char buf[32];
    int  len = 0;

    /* Ctrl+key: map to C0 control codes (0x01-0x1A for A-Z). */
    if ((mods & SOL_MOD_CTRL) && !(mods & SOL_MOD_ALT) &&
        key >= 'A' && key <= 'Z') {
        buf[0] = (char)(key & 0x1Fu);
        len = 1;
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }
    if ((mods & SOL_MOD_CTRL) && !(mods & SOL_MOD_ALT) &&
        key >= 'a' && key <= 'z') {
        buf[0] = (char)((key - 32) & 0x1Fu);
        len = 1;
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }
    /* Ctrl+[ = ESC, Ctrl+\ = 0x1C, Ctrl+] = 0x1D */
    if ((mods & SOL_MOD_CTRL) && key == '[') {
        buf[0] = '\033'; len = 1;
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }
    if ((mods & SOL_MOD_CTRL) && key == '\\') {
        buf[0] = 0x1C; len = 1;
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }
    if ((mods & SOL_MOD_CTRL) && key == ']') {
        buf[0] = 0x1D; len = 1;
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }

    /* Arrow keys — cursor or app mode. */
    const bool app = term->mode_cursor_app;
    int modn = term_modifier_number(mods);
    if (key == SOL_KEY_UP || key == SOL_KEY_DOWN ||
        key == SOL_KEY_RIGHT || key == SOL_KEY_LEFT) {
        char dir = key == SOL_KEY_UP    ? 'A' :
                   key == SOL_KEY_DOWN  ? 'B' :
                   key == SOL_KEY_RIGHT ? 'C' : 'D';
        if (modn > 1) {
            len = snprintf(buf, sizeof(buf), "\033[1;%d%c", modn, dir);
        } else if (app) {
            len = snprintf(buf, sizeof(buf), "\033O%c", dir);
        } else {
            len = snprintf(buf, sizeof(buf), "\033[%c", dir);
        }
        sol_terminal_send_text(term, buf, (size_t)len);
        return;
    }

    /* Other special keys. */
    struct { uint32_t key; const char *normal; int vt_num; } specials[] = {
        { SOL_KEY_HOME,      "\033[H",  1 },
        { SOL_KEY_END,       "\033[F",  4 },
        { SOL_KEY_INSERT,    "\033[2~", 2 },
        { SOL_KEY_DELETE,    "\033[3~", 3 },
        { SOL_KEY_PAGE_UP,   "\033[5~", 5 },
        { SOL_KEY_PAGE_DOWN, "\033[6~", 6 },
    };
    for (size_t i = 0; i < sizeof(specials)/sizeof(specials[0]); ++i) {
        if (key == specials[i].key) {
            if (modn > 1) {
                len = snprintf(buf, sizeof(buf), "\033[%d;%d~",
                               specials[i].vt_num, modn);
            } else {
                len = snprintf(buf, sizeof(buf), "%s", specials[i].normal);
            }
            sol_terminal_send_text(term, buf, (size_t)len);
            return;
        }
    }

    switch (key) {
    case SOL_KEY_BACKSPACE:
        buf[0] = 0x7F; len = 1;
        break;
    case SOL_KEY_ENTER:
        buf[0] = '\r'; len = 1;
        break;
    case SOL_KEY_TAB:
        if (mods & SOL_MOD_SHIFT) {
            buf[0] = '\033'; buf[1] = '['; buf[2] = 'Z'; len = 3;
        } else {
            buf[0] = '\t'; len = 1;
        }
        break;
    default: break;
    }

    if (len > 0) {
        sol_terminal_send_text(term, buf, (size_t)len);
    }
}

void sol_terminal_kill(SolTerminal *term)
{
    if (!term) return;
#if !defined(_WIN32)
    if (term->child_pid > 0) kill(term->child_pid, SIGKILL);
#else
    if (term->hProcess) TerminateProcess(term->hProcess, 0);
#endif
}

bool sol_terminal_is_alive(const SolTerminal *term)
{
    return term ? term->is_alive : false;
}

int sol_terminal_cols(const SolTerminal *term) { return term ? term->cols : 0; }
int sol_terminal_rows(const SolTerminal *term) { return term ? term->rows : 0; }
int sol_terminal_cursor_col(const SolTerminal *term) { return term ? term->cur_col : 0; }
int sol_terminal_cursor_row(const SolTerminal *term) { return term ? term->cur_row : 0; }
bool sol_terminal_cursor_visible(const SolTerminal *term) { return term ? term->cursor_visible : false; }
const char *sol_terminal_title(const SolTerminal *term) { return term ? term->title : "Terminal"; }

int sol_terminal_scrollback_count(const SolTerminal *term)
{
    return term ? term->scrollback_count : 0;
}

int sol_terminal_view_scroll(const SolTerminal *term)
{
    return term ? term->view_scroll : 0;
}

void sol_terminal_set_view_scroll(SolTerminal *term, int offset)
{
    if (!term) return;
    if (offset < 0) offset = 0;
    if (offset > term->scrollback_count) offset = term->scrollback_count;
    term->view_scroll = offset;
}

const SolTermLine *sol_terminal_view_line(const SolTerminal *term, int row)
{
    if (!term || row < 0 || row >= term->rows) return NULL;

    int scrolled = term->view_scroll;
    if (row < scrolled) {
        /* This visual row is in the scrollback. */
        int sb_from_end = scrolled - 1 - row;  /* 0 = most recent */
        int idx = term->scrollback_count - 1 - sb_from_end;
        if (idx < 0) return NULL;
        int actual = (term->scrollback_head + idx) % SOL_TERM_SCROLLBACK_MAX;
        return &term->scrollback[actual];
    }
    /* Viewport row. */
    int screen_row = row - scrolled;
    if (screen_row < 0 || screen_row >= term->rows) return NULL;
    return &term->screen[screen_row];
}
