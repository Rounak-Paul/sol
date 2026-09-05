// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_terminal.c — VT state machine, cell grid, PTY backend, reader thread.
 *
 * VT parser: Paul Williams state machine (vt100.net/emu/dec_ansi_parser).
 * Cell grid: scrollback ring + viewport grid + alt-screen.
 * PTY (Unix):  forkpty/posix_openpt + reader pthread.
 * PTY (Win):   ConPTY (CreatePseudoConsole) + reader thread.
 * SSH (Unix):  libssh2 exec channel over a raw TCP socket + reader thread —
 *              see the "SSH backend" section below. Not yet available on
 *              Windows (BSD-socket assumptions throughout); a Winsock port
 *              is future work, not a design constraint on the Unix path.
 */

#include "sol_terminal.h"
#include "sol_input.h"      /* SOL_KEY_*, SOL_MOD_* constants */
#include "sol_platform.h"   /* sol_platform_now_monotonic_ns */
#include "sol_ssh_config.h" /* SolSshConnection, SolSshAuthMethod */
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
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <libssh2.h>
#endif

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define SOL_TERM_DEFAULT_COLS        80
#define SOL_TERM_DEFAULT_ROWS        24
#define SOL_TERM_MAX_PARAMS          16
#define SOL_TERM_SCROLLBACK_MAX    5000
#define SOL_TERM_OUTPUT_RING_SIZE 524288   /* 512 KB — handles large cmatrix bursts */
#define SOL_TERM_TITLE_MAX           256
#define SOL_TERM_OSC_MAX            1024
/* SOL_TERM_MAX_TABS is defined in sol_terminal.h */

/* Maximum bytes consumed from the ring per drain call.  Bounding this caps
   per-frame VT-parse work so flood output (yes, cmatrix) cannot starve the
   rest of the UI.  Remaining bytes are left in the ring and consumed the next
   frame.  At 60 fps this still processes up to ~3.8 MB/s of terminal output
   before any frames are dropped — far above any realistic interactive use. */
#define SOL_TERM_DRAIN_BYTES_PER_FRAME 65536u

/* Safety cap for synchronized-output mode (DECSET 2026): if an application
   enables it and never sends the closing ?2026l (crash, bug, or hang), the
   terminal must not withhold rendering forever. 200ms is far beyond any
   legitimate single-frame paint and keeps a stuck app from freezing the
   display indefinitely. */
#define SOL_TERM_SYNC_OUTPUT_TIMEOUT_NS 200000000ull

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

/* UTF-8 multi-byte accumulation state for the VT processor.  This lives on the
   terminal, not the stack, because output can be drained in frame-sized chunks
   and a multi-byte sequence may be split across drains. */
typedef struct VtUtf8 {
    uint8_t buf[4];
    int     remaining;
    uint32_t codepoint;
} VtUtf8;

/* ================================================================== */
/* SolTerminal                                                         */
/* ================================================================== */

struct SolTerminal {
    Ca_Instance        *instance;
    SolTerminalManager *manager;    /* owning manager; used to reach OSC 52 callback */

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
    bool         mode_sync_output;  /* DECSET 2026: synchronized update      */
    uint64_t     sync_output_start_ns; /* monotonic ns when 2026 was set     */
    bool         sync_output_closed;   /* one-shot: 2026 turned off this drain */

    /* Mouse reporting. */
    bool         mode_mouse_btn;    /* 1000: button press/release events     */
    bool         mode_mouse_any;    /* 1002: also report motion while button held */
    bool         mode_mouse_sgr;    /* 1006: SGR extended coordinate encoding */

    /* Kitty keyboard protocol (progressive enhancement). Stack per spec;
       depth capped — real apps push 1-2 levels, never unbounded. */
    uint8_t      kitty_flags_stack[8];
    int          kitty_flags_depth;

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
    uint8_t      vt_csi_marker;     /* CSI private-marker byte seen: 0, '?', '<', '=', '>' */

    /* OSC accumulation. */
    char         osc_buf[SOL_TERM_OSC_MAX];
    int          osc_len;
    VtUtf8       utf8_state;

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

    /* SSH transport (only when is_ssh is true — see the "SSH backend"
       section). master_fd stays -1 for an SSH session; ssh_sock is the
       raw TCP socket libssh2 reads/writes through instead. Kept as
       separate fields rather than reusing master_fd for the socket so
       every existing "if (term->master_fd >= 0)" PTY-path check stays
       correct unmodified — those checks now implicitly also mean
       "this is a local PTY session", which is exactly the condition
       they need now that a second transport exists. */
    bool             is_ssh;
    int              ssh_sock;
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel;
#endif

    /* Reader thread. */
    pthread_t    reader_thread;
    bool         reader_started;
    atomic_bool  stop_reader;
    /* Set by reader after each ca_instance_wake(); cleared by drain before
       processing so the reader only wakes once per drain cycle even under
       flood output (e.g. yes, cmatrix at max speed). */
    atomic_bool  wake_pending;

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

    SolTermClipboardWriteFn clipboard_write_fn;
    void                    *clipboard_write_user_data;
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
 * Return true if a codepoint occupies two terminal columns.
 *
 * Covers the East Asian Wide and Fullwidth ranges plus the emoji blocks
 * that terminals conventionally render double-width, per Unicode TR11.
 *
 * cp       Unicode codepoint.
 * Returns  true for double-width codepoints, false otherwise.
 */
/*
 * BMP legacy symbols with default Emoji_Presentation=Yes (Unicode emoji-data.txt):
 * these render as an emoji glyph without needing a VS16 selector, so terminals
 * measure them as double-width. Deliberately narrower than the Misc Symbols
 * (2600-26FF) / Dingbats (2700-27BF) / Misc Symbols and Arrows (2B00-2BFF)
 * blocks that contain them — most codepoints in those blocks are
 * Emoji_Presentation=No (text-default, e.g. suit symbols, plain arrows) and
 * must stay single-width; only VS16 (U+FE0F) would widen those, which this
 * terminal does not track as a separate combining step.
 */
static bool term_codepoint_is_bmp_wide_emoji(uint32_t cp)
{
    return (cp >= 0x231Au && cp <= 0x231Bu) ||  /* watch, hourglass done   */
           (cp >= 0x23E9u && cp <= 0x23ECu) ||  /* fast-forward..fast down */
           cp == 0x23F0u ||                     /* alarm clock             */
           cp == 0x23F3u ||                     /* hourglass not done      */
           (cp >= 0x25FDu && cp <= 0x25FEu) ||  /* medium-small squares    */
           (cp >= 0x2614u && cp <= 0x2615u) ||  /* umbrella, hot beverage  */
           (cp >= 0x2648u && cp <= 0x2653u) ||  /* zodiac (Aries..Pisces)  */
           cp == 0x267Fu ||                     /* wheelchair symbol       */
           cp == 0x2693u ||                     /* anchor                  */
           cp == 0x26A1u ||                     /* high voltage            */
           (cp >= 0x26AAu && cp <= 0x26ABu) ||  /* white/black circle      */
           (cp >= 0x26BDu && cp <= 0x26BEu) ||  /* soccer ball, baseball   */
           (cp >= 0x26C4u && cp <= 0x26C5u) ||  /* snowman, sun+cloud      */
           cp == 0x26CEu ||                     /* Ophiuchus               */
           cp == 0x26D4u ||                     /* no entry                */
           cp == 0x26EAu ||                     /* church                  */
           (cp >= 0x26F2u && cp <= 0x26F3u) ||  /* fountain, flag in hole  */
           cp == 0x26F5u ||                     /* sailboat                */
           cp == 0x26FAu ||                     /* tent                    */
           cp == 0x26FDu ||                     /* fuel pump               */
           cp == 0x2705u ||                     /* check mark button       */
           (cp >= 0x270Au && cp <= 0x270Bu) ||  /* raised fist, raised hand*/
           cp == 0x2728u ||                     /* sparkles                */
           cp == 0x274Cu ||                     /* cross mark              */
           cp == 0x274Eu ||                     /* cross mark button       */
           (cp >= 0x2753u && cp <= 0x2755u) ||  /* red question..white excl*/
           cp == 0x2757u ||                     /* red exclamation mark    */
           (cp >= 0x2795u && cp <= 0x2797u) ||  /* plus, minus, divide     */
           cp == 0x27B0u ||                     /* curly loop              */
           cp == 0x27BFu ||                     /* double curly loop       */
           (cp >= 0x2B1Bu && cp <= 0x2B1Cu) ||  /* black/white large square*/
           cp == 0x2B50u ||                     /* star                    */
           cp == 0x2B55u;                       /* hollow red circle       */
}

static bool term_codepoint_is_wide(uint32_t cp)
{
    return (cp >= 0x1100u  && cp <= 0x115Fu) ||  /* Hangul Jamo init.    */
           (cp >= 0x2E80u  && cp <= 0x303Eu) ||  /* CJK radicals, Kangxi */
           (cp >= 0x3041u  && cp <= 0x33FFu) ||  /* Kana, CJK compat     */
           (cp >= 0x3400u  && cp <= 0x4DBFu) ||  /* CJK ext A            */
           (cp >= 0x4E00u  && cp <= 0x9FFFu) ||  /* CJK unified          */
           (cp >= 0xA000u  && cp <= 0xA4CFu) ||  /* Yi                   */
           (cp >= 0xAC00u  && cp <= 0xD7A3u) ||  /* Hangul syllables     */
           (cp >= 0xF900u  && cp <= 0xFAFFu) ||  /* CJK compat ideograph */
           (cp >= 0xFE30u  && cp <= 0xFE6Fu) ||  /* CJK compat forms     */
           (cp >= 0xFF00u  && cp <= 0xFF60u) ||  /* Fullwidth forms      */
           (cp >= 0xFFE0u  && cp <= 0xFFE6u) ||  /* Fullwidth signs      */
           term_codepoint_is_bmp_wide_emoji(cp) ||
           (cp >= 0x1F300u && cp <= 0x1F64Fu) || /* Misc symbols, emoji  */
           (cp >= 0x1F680u && cp <= 0x1F6FFu) || /* Transport and map    */
           (cp >= 0x1F900u && cp <= 0x1F9FFu) || /* Supplemental symbols */
           (cp >= 0x1FA00u && cp <= 0x1FAFFu) || /* Symbols/Pictographs Ext-A */
           (cp >= 0x20000u && cp <= 0x3FFFDu);   /* CJK ext B and beyond */
}

/*
 * Write a codepoint into the current cursor cell and advance the cursor.
 *
 * Double-width codepoints occupy two cells: the lead cell carries the
 * codepoint and SOL_TERM_ATTR_WIDE, the trailing cell is marked
 * SOL_TERM_ATTR_WIDE_TAIL and is skipped by the renderer. A wide glyph
 * that would straddle the right margin wraps to the next line instead of
 * being split.
 *
 * term       Terminal.
 * codepoint  Unicode codepoint to write.
 */
static void term_put_char(SolTerminal *term, uint32_t codepoint)
{
    const bool wide = term_codepoint_is_wide(codepoint);
    const int  width = wide ? 2 : 1;

    if (term->pending_wrap && term->mode_autowrap) {
        term->screen[term->cur_row].cells[term->cols - 1].attrs &=
            (uint16_t)~SOL_TERM_ATTR_WIDE;
        term_new_line(term);
        term->cur_col = 0;
    }
    term->pending_wrap = false;

    /* A wide glyph cannot straddle the right margin. */
    if (width == 2 && term->cur_col == term->cols - 1) {
        if (term->mode_autowrap && term->cols >= 2) {
            term_new_line(term);
            term->cur_col = 0;
        } else {
            /* A single-column viewport has no room for a wide glyph even
               after wrapping — drop it rather than write past the row. */
            return;
        }
    }

    SolTermLine *line = &term->screen[term->cur_row];
    SolTermCell cell  = term->cur_attrs;
    cell.codepoint    = codepoint;
    if (wide) cell.attrs |= SOL_TERM_ATTR_WIDE;

    if (term->mode_insert && term->cur_col < term->cols - width) {
        memmove(&line->cells[term->cur_col + width],
                &line->cells[term->cur_col],
                (size_t)(term->cols - term->cur_col - width) * sizeof(SolTermCell));
    }

    line->cells[term->cur_col] = cell;
    if (wide && term->cur_col + 1 < term->cols) {
        SolTermCell tail = term->cur_attrs;
        tail.codepoint   = 0u;
        tail.attrs      |= SOL_TERM_ATTR_WIDE_TAIL;
        line->cells[term->cur_col + 1] = tail;
    }
    line->dirty = true;
    term->dirty = true;

    if (term->cur_col >= term->cols - width) {
        term->cur_col   = term->cols - 1;
        term->pending_wrap = true;
    } else {
        term->cur_col += width;
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
    case 1000: /* mouse button press/release tracking */
        term->mode_mouse_btn = set;
        break;
    case 1002: /* mouse button + motion-while-pressed tracking */
        term->mode_mouse_any = set;
        break;
    case 1006: /* SGR extended coordinate encoding */
        term->mode_mouse_sgr = set;
        break;
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
    case 2026: /* synchronized output (mode 2026) */
        if (set) {
            term->mode_sync_output    = true;
            term->sync_output_start_ns = sol_platform_now_monotonic_ns();
        } else if (term->mode_sync_output) {
            term->mode_sync_output   = false;
            term->sync_output_closed = true;
        }
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
            const char *da = "\033[?6c";
            sol_terminal_send_text(term, da, strlen(da));
        }
        break;
    case 'n': /* DSR: device status report */
        if (vt_param(term, 0, 0) == 6) {
            /* CPR: cursor position report */
            char buf[32];
            snprintf(buf, sizeof(buf), "\033[%d;%dR",
                     term->cur_row + 1, term->cur_col + 1);
            sol_terminal_send_text(term, buf, strlen(buf));
        }
        break;

    /* ---- Kitty keyboard protocol (progressive enhancement) ----
       All four forms end in final byte 'u' with a marker byte ('?' query,
       '>' push, '<' pop, '=' set) captured during CSI_PARAM. A bare "CSI u"
       with no marker is the older SCORC (restore cursor) sequence, handled
       by the other case 'u' above — merged into one case since C forbids
       duplicate case labels. Only bit 0 (disambiguate escape codes) changes
       actual key encoding; other requested bits are accepted in the
       reported flags but do not change what Sol sends, since Sol does not
       implement event-type/text-association reporting. */
    case 'u':
        if (term->vt_csi_marker == 0) {
            term->cur_row   = term->saved_row;
            term->cur_col   = term->saved_col;
            term->cur_attrs = term->saved_attrs;
            term->pending_wrap = false;
        } else if (term->vt_csi_marker == '?') {
            /* Query: report the top of the enhancement stack (0 if empty). */
            const uint8_t flags = term->kitty_flags_depth > 0
                ? term->kitty_flags_stack[term->kitty_flags_depth - 1] : 0u;
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "\033[?%uu", (unsigned)flags);
            if (len > 0) sol_terminal_send_text(term, buf, (size_t)len);
        } else if (term->vt_csi_marker == '>') {
            /* Push: new entry defaults to all-zero flags until a following
               '=' sets them, matching a bare push observed in the wild. */
            if (term->kitty_flags_depth <
                (int)(sizeof(term->kitty_flags_stack) / sizeof(term->kitty_flags_stack[0]))) {
                term->kitty_flags_stack[term->kitty_flags_depth++] = 0u;
            }
        } else if (term->vt_csi_marker == '<') {
            /* Pop N entries (default 1). */
            int n = vt_param(term, 0, 1);
            while (n-- > 0 && term->kitty_flags_depth > 0)
                term->kitty_flags_depth--;
        } else if (term->vt_csi_marker == '=') {
            /* Set: Pflags ; Pmode (mode 1=set/replace, 2=set all requested
               bits, 3=reset requested bits; default 1). Applies to the top
               of stack, pushing a base entry first if the stack is empty
               so a set without a prior push still has somewhere to live. */
            if (term->kitty_flags_depth == 0) {
                term->kitty_flags_stack[term->kitty_flags_depth++] = 0u;
            }
            uint8_t *top = &term->kitty_flags_stack[term->kitty_flags_depth - 1];
            const uint8_t requested = (uint8_t)(vt_param(term, 0, 0) & 0x1F);
            switch (vt_param(term, 1, 1)) {
            case 2:  *top |= requested;  break;   /* set all requested bits */
            case 3:  *top &= (uint8_t)~requested; break; /* reset requested bits */
            default: *top = requested;  break;    /* replace */
            }
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
        /* A scroll region needs at least two rows to be meaningful; at
           rows < 2, term_clamp(v, 0, term->rows - 2) is asked to clamp
           into an inverted [0, -1] range and returns -1, corrupting
           margin_top into an out-of-bounds row index that the next
           linefeed's scroll-up would index the screen array with
           (screen[-1]). Skip DECSTBM entirely at rows < 2, matching the
           existing cols < 2 guard used elsewhere for wide-char writes. */
        if (!priv && term->rows >= 2) {
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
            term->kitty_flags_depth = 0;
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

/* Decode a standard base64 payload of `len` bytes in place. Returns the
   decoded byte count, or 0 on malformed input (odd padding, invalid
   alphabet) — callers must treat 0 as "ignore this OSC 52 request" rather
   than emit partial/garbage clipboard content. `out` may alias `in`. */
static size_t vt_base64_decode(const char *in, size_t len, char *out, size_t out_cap)
{
    /* Built once on first call: standard base64 alphabet, everything else -1.
       (Not the GNU `[lo ... hi] = val` designated-range syntax MSVC rejects.) */
    static int8_t T[256];
    static bool   T_ready = false;
    if (!T_ready) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        for (int i = 0; i < 26; ++i) {
            T['A' + i] = (int8_t)i;
            T['a' + i] = (int8_t)(26 + i);
        }
        for (int i = 0; i < 10; ++i) T['0' + i] = (int8_t)(52 + i);
        T['+'] = 62;
        T['/'] = 63;
        T_ready = true;
    }
    size_t out_len = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '=') break;              /* padding: stop */
        const int8_t v = T[c];
        if (v < 0) continue;              /* skip whitespace/newlines per RFC leniency */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= out_cap) return 0;
            out[out_len++] = (char)((acc >> bits) & 0xFFu);
        }
    }
    return out_len;
}

static void vt_osc_dispatch(SolTerminal *term)
{
    /* Format: "Pn;text" where Pn is the OSC command number. */
    char *semi = (char *)memchr(term->osc_buf, ';', (size_t)term->osc_len);
    if (!semi) return;
    int cmd = (int)strtol(term->osc_buf, NULL, 10);
    const char *text = semi + 1;
    size_t text_len = (size_t)term->osc_len - (size_t)(text - term->osc_buf);

    switch (cmd) {
    case 0: /* Set icon name and window title */
    case 1: /* Set icon name (treat same as title) */
    case 2: /* Set window title */
        {
            size_t len = text_len;
            if (len >= SOL_TERM_TITLE_MAX) len = SOL_TERM_TITLE_MAX - 1;
            memcpy(term->title, text, len);
            term->title[len] = '\0';
            term->dirty = true;
        }
        break;
    case 52: /* Clipboard write: OSC 52 ; Pc ; base64(Pd) */
        {
            /* Pc (selection target) is accepted but not distinguished —
               Sol has one system clipboard. Only a write is supported;
               a query payload ("?") is silently ignored rather than
               answered, matching common conservative terminal behavior
               (no read-back channel for arbitrary TUI programs). */
            char *inner_semi = (char *)memchr(text, ';', text_len);
            const char *payload = inner_semi ? inner_semi + 1 : text;
            size_t payload_len = inner_semi
                ? text_len - (size_t)(inner_semi + 1 - text)
                : text_len;
            if (payload_len == 0 || (payload_len == 1 && payload[0] == '?')) break;

            SolTerminalManager *mgr = term->manager;
            if (!mgr || !mgr->clipboard_write_fn) break;

            char decoded[SOL_TERM_OSC_MAX];
            size_t n = vt_base64_decode(payload, payload_len, decoded, sizeof(decoded) - 1u);
            if (n == 0) break;
            decoded[n] = '\0';
            mgr->clipboard_write_fn(decoded, mgr->clipboard_write_user_data);
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
        term->vt_csi_marker     = 0;
        term->vt_state          = VT_CSI_PARAM;
        /* Fall through to process `byte` in CSI_PARAM. */
        /* FALLTHROUGH */
    case VT_CSI_PARAM:
        if (byte == '?') {
            term->vt_dcs_private = true;
            term->vt_csi_marker  = byte;
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
        } else if (byte == 0x3C || byte == 0x3D || byte == 0x3E) {
            /* '<' '=' '>' — Kitty keyboard protocol pop/set/push prefixes
               (final byte 'u'); recorded so vt_csi_dispatch can tell them
               apart. No other CSI sequence Sol implements uses these. */
            term->vt_csi_marker = byte;
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
 * UTF-8 decoding applies only to GROUND state. Escape, CSI, OSC and DCS
 * sequences are byte-oriented: a high byte inside an OSC string (such as a
 * non-ASCII path in a title) must reach the sequence handler verbatim
 * rather than being consumed by the multi-byte accumulator.
 *
 * term  Terminal.
 * u     UTF-8 accumulator state.
 * byte  Incoming byte.
 */
static void vt_utf8_feed(SolTerminal *term, VtUtf8 *u, uint8_t byte)
{
    if (term->vt_state != VT_GROUND) {
        if (u->remaining > 0) vt_utf8_reset(u);
        vt_process_byte(term, byte);
        return;
    }

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
        /* Invalid continuation — discard the partial codepoint and
           reprocess this byte as a fresh lead byte or control. */
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
 * Deposit n freshly-read bytes into the output ring buffer and, if the
 * main thread has consumed the previous wake, ping the Causality
 * instance so it drains and repaints. Shared by both the local-PTY and
 * SSH reader threads — every byte-source difference between them ends
 * here.
 *
 * term  Terminal whose ring buffer receives the bytes.
 * buf   Freshly-read bytes.
 * n     Number of bytes in buf (> 0).
 */
static void sol_terminal_reader_deposit(SolTerminal *term, const char *buf, ssize_t n)
{
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

    /* Rate-limit wakes: only call ca_instance_wake() if the main thread
       has already consumed the previous wake (i.e. cleared wake_pending
       during drain).  Under flood output this keeps wake rate at one per
       drain cycle (~one per display frame) instead of one per read(). */
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(
            &term->wake_pending, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        ca_instance_wake();
    }
}

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
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(1000);
                continue;
            }
            break; /* EIO (slave closed), EBADF (fd closed), or EOF */
        }

        sol_terminal_reader_deposit(term, buf, n);
    }

    term->is_alive = false;
    ca_instance_wake();
    return NULL;
}

/*
 * Reader thread for an SSH-backed session: blocks on
 * libssh2_channel_read_ex, deposits bytes into the ring buffer, then
 * wakes the Causality instance. Mirrors sol_terminal_reader_thread
 * exactly except for the byte source — see sol_terminal_reader_deposit.
 *
 * arg  SolTerminal pointer.
 * Returns NULL always.
 */
static void *sol_terminal_ssh_reader_thread(void *arg)
{
    SolTerminal *term = (SolTerminal *)arg;
    char buf[32768];
    LIBSSH2_CHANNEL *channel = term->ssh_channel;

    while (!atomic_load_explicit(&term->stop_reader, memory_order_relaxed)) {
        ssize_t n = (ssize_t)libssh2_channel_read_ex(channel, 0, buf, sizeof(buf));
        if (n > 0) {
            sol_terminal_reader_deposit(term, buf, n);
            continue;
        }
        if (n == LIBSSH2_ERROR_EAGAIN) {
            usleep(1000);
            continue;
        }
        /* n == 0 (remote closed its write side) or a real libssh2 error
           (n < 0, not EAGAIN) — either way, this session is over. Also
           stop once the remote has sent SSH_MSG_CHANNEL_EOF even if a
           final zero-byte read hasn't been observed yet, so a channel
           that stops sending data but never reports 0/error doesn't
           spin this thread forever. */
        if (n <= 0 && (n != LIBSSH2_ERROR_EAGAIN)) break;
        if (libssh2_channel_eof(channel)) break;
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
 * cwd   Initial working directory for the child shell, or NULL to inherit.
 * Returns true on success.
 */
static bool sol_terminal_start_pty(SolTerminal *term, const char *cwd)
{
    struct winsize ws = { 0 };
    ws.ws_col = (unsigned short)term->cols;
    ws.ws_row = (unsigned short)term->rows;

    const char *shell = getenv("SHELL");
    if (!shell || shell[0] == '\0') shell = "/bin/sh";

    pid_t pid = forkpty(&term->master_fd, NULL, NULL, &ws);
    if (pid < 0) return false;

    if (pid == 0) {
        /* Child: optionally change to project root before exec. */
        if (cwd && cwd[0] != '\0')
            chdir(cwd);
        const char *argv[] = { shell, NULL };
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execvp(shell, (char *const *)argv);
        _exit(127);
    }

    term->child_pid = pid;

    /* Keep the PTY master in blocking mode.  The reader thread owns blocking
       reads; transient non-blocking EAGAIN would otherwise look like EOF and
       leave the terminal with only a cursor and no shell output. */
    int flags = fcntl(term->master_fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK))
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

    if (term->child_pid > 0) {
        kill(term->child_pid, SIGHUP);
        kill(term->child_pid, SIGTERM);

        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 20; ++i) {
            pid_t r = waitpid(term->child_pid, &status, WNOHANG);
            if (r == term->child_pid || (r < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            kill(term->child_pid, SIGKILL);
            while (waitpid(term->child_pid, &status, 0) < 0 && errno == EINTR) {}
        }
        term->child_pid = 0;
    }

    /* Reaping the child should close the slave side and wake read(). Closing
       the master before join is only a fallback for already-dead children
       where no process is left to close the slave. */
    if (term->reader_started && term->child_pid == 0 && term->master_fd >= 0) {
        int fd = term->master_fd;
        term->master_fd = -1;
        close(fd);
    }
    if (term->reader_started) {
        pthread_join(term->reader_thread, NULL);
        term->reader_started = false;
    }

    if (term->master_fd >= 0) {
        close(term->master_fd);
        term->master_fd = -1;
    }
}

/* ================================================================== */
/* SSH backend — Unix                                                  */
/* ================================================================== */

/*
 * Resolve host to an IPv4/IPv6 address and open a connected, blocking
 * TCP socket to it on the given port.
 *
 * host       Hostname or numeric address.
 * port       TCP port (SSH is almost always 22, but the connection
 *            profile carries whatever the user configured).
 * out_error  Set to a short static string describing the failure on
 *            return -1; untouched on success.
 * Returns    A connected socket fd, or -1 on failure.
 */
static int sol_ssh_connect_tcp(const char *host, uint16_t port, const char **out_error)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_str, &hints, &results) != 0 || !results) {
        if (out_error) *out_error = "could not resolve host";
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *ai = results; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(results);

    if (sock < 0 && out_error) *out_error = "could not connect to host";
    return sock;
}

/*
 * Verify the remote host's key against ~/.ssh/known_hosts.
 *
 * Sol never silently trusts an unrecognized or changed host key — an
 * unknown host fails the connection with a clear message (the user can
 * still add it via a normal `ssh` invocation first, which is the
 * standard trust-on-first-use flow every SSH client already asks the
 * user to go through once) rather than proceeding as if the key were
 * verified. A file that cannot be read at all (missing known_hosts, no
 * $HOME) is treated the same as "not found" for the one host being
 * checked, not as a hard error — a first-ever SSH connection from this
 * machine should not be blocked by an absent file.
 *
 * session    Handshaked libssh2 session to read the host key from.
 * host       Hostname the connection was made to (the known_hosts key).
 * port       Port the connection was made to.
 * out_error  Set to a short static string describing the failure on
 *            return false; untouched on success.
 * Returns    true if the host key matches a known_hosts entry.
 */
static bool sol_ssh_verify_host_key(LIBSSH2_SESSION *session, const char *host,
                                    uint16_t port, const char **out_error)
{
    size_t key_len = 0;
    int key_type = 0;
    const char *key = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key) {
        if (out_error) *out_error = "remote sent no host key";
        return false;
    }

    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        if (out_error) *out_error = "could not initialize known_hosts check";
        return false;
    }

    bool ok = false;
    char *home = getenv("HOME");
    if (home) {
        char path[1088];
        snprintf(path, sizeof(path), "%s/.ssh/known_hosts", home);
        /* A missing/unreadable file just means nothing is known yet —
           libssh2_knownhost_checkp below correctly reports NOTFOUND in
           that case too, so there is nothing to branch on here. */
        (void)libssh2_knownhost_readfile(hosts, path, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }

    struct libssh2_knownhost *found = NULL;
    const int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
        ((key_type == LIBSSH2_HOSTKEY_TYPE_RSA)     ? LIBSSH2_KNOWNHOST_KEY_SSHRSA
        : (key_type == LIBSSH2_HOSTKEY_TYPE_DSS)    ? LIBSSH2_KNOWNHOST_KEY_SSHDSS
        : (key_type == LIBSSH2_HOSTKEY_TYPE_ECDSA_256) ? LIBSSH2_KNOWNHOST_KEY_ECDSA_256
        : (key_type == LIBSSH2_HOSTKEY_TYPE_ECDSA_384) ? LIBSSH2_KNOWNHOST_KEY_ECDSA_384
        : (key_type == LIBSSH2_HOSTKEY_TYPE_ECDSA_521) ? LIBSSH2_KNOWNHOST_KEY_ECDSA_521
        : (key_type == LIBSSH2_HOSTKEY_TYPE_ED25519)   ? LIBSSH2_KNOWNHOST_KEY_ED25519
        : LIBSSH2_KNOWNHOST_KEY_UNKNOWN);
    const int check = libssh2_knownhost_checkp(hosts, host, (int)port, key, key_len,
                                               mask | LIBSSH2_KNOWNHOST_TYPE_PLAIN,
                                               &found);
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        ok = true;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        if (out_error) *out_error =
            "host key changed — possible impersonation; refusing to connect "
            "(remove the stale entry from ~/.ssh/known_hosts if this is expected)";
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        if (out_error) *out_error =
            "host key not in ~/.ssh/known_hosts — connect with a regular `ssh` "
            "client first to add it, then retry";
        break;
    default:
        if (out_error) *out_error = "could not verify host key";
        break;
    }

    libssh2_knownhost_free(hosts);
    return ok;
}

/*
 * Authenticate an established, handshaked libssh2 session using the
 * method recorded on conn.
 *
 * session    Handshaked session to authenticate.
 * conn       Connection profile (user, auth method, key path).
 * password   Password to try (only used for SOL_SSH_AUTH_PASSWORD).
 * out_error  Set to a short static string describing the failure on
 *            return false; untouched on success.
 * Returns    true once libssh2_userauth_authenticated(session) is true.
 */
static bool sol_ssh_authenticate(LIBSSH2_SESSION *session, const SolSshConnection *conn,
                                 const char *password, const char **out_error)
{
    switch (conn->auth) {
    case SOL_SSH_AUTH_PASSWORD:
        if (libssh2_userauth_password(session, conn->user, password ? password : "") != 0) {
            if (out_error) *out_error = "password authentication failed";
            return false;
        }
        break;

    case SOL_SSH_AUTH_KEY: {
        if (conn->key_path[0] == '\0') {
            if (out_error) *out_error = "no private key file configured";
            return false;
        }
        /* Public key path left NULL: libssh2 derives it from the
           private key itself for the common case (OpenSSH-format keys
           embed/derive their own public half), matching what a bare
           `ssh -i keyfile` invocation does without a separate .pub. */
        if (libssh2_userauth_publickey_fromfile(
                session, conn->user, NULL, conn->key_path, NULL) != 0) {
            if (out_error) *out_error = "public key authentication failed "
                                         "(wrong passphrase, or key rejected)";
            return false;
        }
        break;
    }

    case SOL_SSH_AUTH_AGENT: {
        LIBSSH2_AGENT *agent = libssh2_agent_init(session);
        if (!agent) {
            if (out_error) *out_error = "could not initialize ssh-agent connection";
            return false;
        }
        bool ok = false;
        if (libssh2_agent_connect(agent) == 0 &&
            libssh2_agent_list_identities(agent) == 0) {
            struct libssh2_agent_publickey *identity = NULL;
            struct libssh2_agent_publickey *prev = NULL;
            for (;;) {
                if (libssh2_agent_get_identity(agent, &identity, prev) != 0) break;
                if (libssh2_agent_userauth(agent, conn->user, identity) == 0) {
                    ok = true;
                    break;
                }
                prev = identity;
            }
        }
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        if (!ok) {
            if (out_error) *out_error =
                "no identity offered by ssh-agent was accepted "
                "(is ssh-agent running with the right key loaded?)";
            return false;
        }
        break;
    }
    }

    return libssh2_userauth_authenticated(session) != 0;
}

/*
 * Open an SSH session to conn's host, authenticate, open a channel,
 * request a PTY sized to term->cols/rows, and start an interactive
 * shell on it — the SSH equivalent of sol_terminal_start_pty. Sets
 * ssh_sock/ssh_session/ssh_channel and starts the reader thread.
 *
 * term       Terminal to initialise. term->cols/rows must already be set
 *            (sol_terminal_create sets the defaults before calling this).
 * conn       Connection profile to connect with.
 * password   Password to try (only used for SOL_SSH_AUTH_PASSWORD).
 * out_error  Set to a short, user-presentable string on failure
 *            (points at either a static string or a small internal
 *            static buffer — valid until the next call on any thread,
 *            so callers must copy it before doing anything else that
 *            might call into this module again). Untouched on success.
 * Returns    true on success.
 */
static bool sol_terminal_start_ssh(SolTerminal *term, const SolSshConnection *conn,
                                   const char *password, const char **out_error)
{
    static char err_buf[256];

    term->ssh_sock = sol_ssh_connect_tcp(conn->host, conn->port, out_error);
    if (term->ssh_sock < 0) return false;

    term->ssh_session = libssh2_session_init();
    if (!term->ssh_session) {
        if (out_error) *out_error = "could not create SSH session";
        close(term->ssh_sock);
        term->ssh_sock = -1;
        return false;
    }
    /* Reader thread owns blocking reads on the channel, same contract
       as the local-PTY reader thread — see its comment. Handshake and
       auth below run blocking too, which is intended: they happen
       synchronously on the main thread before the terminal is usable
       either way, so there is nothing to overlap them with. */
    libssh2_session_set_blocking(term->ssh_session, 1);

    if (libssh2_session_handshake(term->ssh_session, term->ssh_sock) != 0) {
        if (out_error) *out_error = "SSH handshake failed";
        libssh2_session_free(term->ssh_session);
        term->ssh_session = NULL;
        close(term->ssh_sock);
        term->ssh_sock = -1;
        return false;
    }

    if (!sol_ssh_verify_host_key(term->ssh_session, conn->host, conn->port, out_error))
        goto fail_after_handshake;

    if (!sol_ssh_authenticate(term->ssh_session, conn, password, out_error))
        goto fail_after_handshake;

    term->ssh_channel = libssh2_channel_open_session(term->ssh_session);
    if (!term->ssh_channel) {
        if (out_error) *out_error = "could not open SSH channel";
        goto fail_after_handshake;
    }

    if (libssh2_channel_request_pty_ex(term->ssh_channel, "xterm-256color",
                                       sizeof("xterm-256color") - 1, NULL, 0,
                                       term->cols, term->rows, 0, 0) != 0) {
        if (out_error) *out_error = "remote refused to allocate a PTY";
        goto fail_after_channel;
    }

    if (libssh2_channel_shell(term->ssh_channel) != 0) {
        if (out_error) *out_error = "remote refused to start a shell";
        goto fail_after_channel;
    }

    /* Reader thread wants blocking channel reads with an EAGAIN escape
       hatch handled explicitly (see sol_terminal_ssh_reader_thread),
       not libssh2's fully-nonblocking session mode — leave the session
       itself blocking; libssh2_channel_read_ex on a blocking session
       still returns LIBSSH2_ERROR_EAGAIN from within a callback-driven
       keepalive/window-adjust path in rare cases, which the reader
       already treats as "try again", so this is correct either way. */
    atomic_store_explicit(&term->stop_reader, false, memory_order_relaxed);
    if (pthread_create(&term->reader_thread, NULL,
                       sol_terminal_ssh_reader_thread, term) != 0) {
        if (out_error) *out_error = "could not start terminal reader thread";
        goto fail_after_channel;
    }
    term->reader_started = true;
    term->is_ssh = true;
    return true;

fail_after_channel:
    libssh2_channel_free(term->ssh_channel);
    term->ssh_channel = NULL;
fail_after_handshake:
    if (out_error && *out_error) {
        /* Copy into a stable buffer before session_free — some libssh2
           error strings referenced no error buffer of our own, but
           sol_ssh_authenticate/sol_ssh_verify_host_key already only
           ever hand back static string literals, so this snprintf is a
           belt-and-suspenders normalization, not a use-after-free fix. */
        snprintf(err_buf, sizeof(err_buf), "%s", *out_error);
        *out_error = err_buf;
    }
    libssh2_session_disconnect_ex(term->ssh_session, SSH_DISCONNECT_BY_APPLICATION,
                                  "", "");
    libssh2_session_free(term->ssh_session);
    term->ssh_session = NULL;
    close(term->ssh_sock);
    term->ssh_sock = -1;
    return false;
}

/*
 * Tear down an SSH-backed session: stop the reader thread, close the
 * channel and session, and close the socket. The SSH equivalent of
 * sol_terminal_stop_pty.
 *
 * term  Terminal to tear down.
 */
static void sol_terminal_stop_ssh(SolTerminal *term)
{
    atomic_store_explicit(&term->stop_reader, true, memory_order_relaxed);

    /* Closing the raw socket first — NOT libssh2_channel_close — is
       what actually unblocks the reader thread. The reader is parked
       inside libssh2_channel_read_ex, which internally calls select()
       on ssh_sock whenever it would otherwise block; closing the fd out
       from under that select() makes it return immediately with an
       error, so libssh2_channel_read_ex returns and the reader thread
       exits its loop. libssh2_channel_close(), by contrast, sends a
       protocol-level close message through the very channel object the
       reader thread may be mid-call on — calling it here would be an
       unsynchronized concurrent use of the same LIBSSH2_CHANNEL from
       two threads (this one and the reader), and empirically does not
       unblock a reader stuck in select() anyway: verified this
       deadlocked pthread_join indefinitely against a real server
       before switching to closing the socket first. */
    if (term->ssh_sock >= 0) {
        close(term->ssh_sock);
        term->ssh_sock = -1;
    }
    if (term->reader_started) {
        pthread_join(term->reader_thread, NULL);
        term->reader_started = false;
    }
    /* Now safe: the reader thread has exited, so this is the only
       thread touching ssh_channel/ssh_session from here on. The socket
       is already closed, so these calls cannot perform real network
       I/O — they only free libssh2's in-memory state — but they still
       need to run to release that state without leaking it. */
    if (term->ssh_channel) {
        libssh2_channel_free(term->ssh_channel);
        term->ssh_channel = NULL;
    }
    if (term->ssh_session) {
        libssh2_session_free(term->ssh_session);
        term->ssh_session = NULL;
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

        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &term->wake_pending, &expected, true,
                memory_order_acq_rel, memory_order_relaxed)) {
            ca_instance_wake();
        }
    }

    term->is_alive = false;
    ca_instance_wake();
    return NULL;
}

static bool sol_terminal_start_pty(SolTerminal *term, const char *cwd)
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
    wchar_t cwd_wide[MAX_PATH];
    LPCWSTR cwd_arg = NULL;
    if (cwd && *cwd) {
        if (MultiByteToWideChar(CP_UTF8, 0, cwd, -1, cwd_wide, MAX_PATH) > 0) {
            cwd_arg = cwd_wide;
        }
    }
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, cwd_arg,
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
 * cwd       Initial working directory, or NULL to inherit.
 * Returns   Heap-allocated terminal, or NULL on failure.
 */
/*
 * Allocate and initialise every transport-agnostic part of a terminal
 * session (VT/grid state, screen lines, output ring mutex) — everything
 * sol_terminal_create and sol_terminal_create_ssh both need before
 * launching their respective backend.
 *
 * instance  Causality instance the terminal belongs to.
 * Returns   A freshly allocated, transport-less SolTerminal, or NULL on
 *           allocation failure. Caller must still launch a backend
 *           (sol_terminal_start_pty/start_ssh) before the session is
 *           usable, and must free via the matching cleanup path on
 *           backend-launch failure (screen lines + mutex only — no
 *           backend to tear down yet).
 */
static SolTerminal *sol_terminal_create_base(Ca_Instance *instance)
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
    term->ssh_sock  = -1;
#endif

    pthread_mutex_init(&term->output_mutex, NULL);

    /* Allocate screen lines. */
    for (int r = 0; r < term->rows; ++r) {
        if (!term_line_alloc(&term->screen[r], term->cols)) {
            /* Cleanup on partial allocation. */
            for (int i = 0; i < r; ++i) term_line_free(&term->screen[i]);
            pthread_mutex_destroy(&term->output_mutex);
            free(term);
            return NULL;
        }
    }

    return term;
}

static SolTerminal *sol_terminal_create(Ca_Instance *instance, const char *cwd)
{
    SolTerminal *term = sol_terminal_create_base(instance);
    if (!term) return NULL;

    if (!sol_terminal_start_pty(term, cwd)) {
        for (int r = 0; r < term->rows; ++r) term_line_free(&term->screen[r]);
        pthread_mutex_destroy(&term->output_mutex);
        free(term);
        return NULL;
    }

    return term;
}

#if !defined(_WIN32)
/*
 * Create a terminal session backed by an SSH shell channel instead of a
 * local PTY — see sol_terminal_start_ssh for the connect/auth/channel
 * flow. Not available on Windows in this pass (see this file's header
 * comment).
 *
 * instance   Causality instance the terminal belongs to.
 * conn       Connection profile to connect with.
 * password   Password to try (only used for SOL_SSH_AUTH_PASSWORD).
 * out_error  Set to a user-presentable failure string on NULL return
 *            (see sol_terminal_start_ssh's contract); untouched on
 *            success.
 * Returns    A live terminal session, or NULL on failure.
 */
static SolTerminal *sol_terminal_create_ssh(Ca_Instance *instance,
                                            const SolSshConnection *conn,
                                            const char *password,
                                            const char **out_error)
{
    SolTerminal *term = sol_terminal_create_base(instance);
    if (!term) {
        if (out_error) *out_error = "out of memory";
        return NULL;
    }

    if (!sol_terminal_start_ssh(term, conn, password, out_error)) {
        for (int r = 0; r < term->rows; ++r) term_line_free(&term->screen[r]);
        pthread_mutex_destroy(&term->output_mutex);
        free(term);
        return NULL;
    }

    snprintf(term->title, sizeof(term->title), "%s", conn->name[0] ? conn->name : conn->host);
    return term;
}
#endif

/*
 * Destroy a terminal session, stopping the PTY and freeing all memory.
 *
 * term  Terminal to destroy (safe to call with NULL).
 */
static void sol_terminal_destroy(SolTerminal *term)
{
    if (!term) return;
#if !defined(_WIN32)
    if (term->is_ssh) {
        sol_terminal_stop_ssh(term);
    } else {
        sol_terminal_stop_pty(term);
    }
#else
    sol_terminal_stop_pty(term);
#endif
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

void sol_terminal_manager_set_clipboard_write(SolTerminalManager *mgr,
                                              SolTermClipboardWriteFn fn,
                                              void *user_data)
{
    if (!mgr) return;
    mgr->clipboard_write_fn        = fn;
    mgr->clipboard_write_user_data = user_data;
}

SolTerminal *sol_terminal_manager_new_tab(SolTerminalManager *mgr,
                                          const char *cwd)
{
    if (!mgr || mgr->tab_count >= SOL_TERM_MAX_TABS) return NULL;
    SolTerminal *term = sol_terminal_create(mgr->instance, cwd);
    if (!term) return NULL;
    term->manager = mgr;
    mgr->tabs[mgr->tab_count++] = term;
    mgr->active_index = mgr->tab_count - 1;
    return term;
}

#if !defined(_WIN32)
SolTerminal *sol_terminal_manager_new_ssh_tab(SolTerminalManager *mgr,
                                              const SolSshConnection *conn,
                                              const char *password,
                                              const char **out_error)
{
    if (!mgr || !conn) {
        if (out_error) *out_error = "invalid arguments";
        return NULL;
    }
    if (mgr->tab_count >= SOL_TERM_MAX_TABS) {
        if (out_error) *out_error = "too many terminal tabs already open";
        return NULL;
    }
    SolTerminal *term = sol_terminal_create_ssh(mgr->instance, conn, password, out_error);
    if (!term) return NULL;
    term->manager = mgr;
    mgr->tabs[mgr->tab_count++] = term;
    mgr->active_index = mgr->tab_count - 1;
    return term;
}
#endif

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

        /* Clear wake_pending before touching the ring so the reader thread
           can schedule the next wake as soon as new bytes arrive after we
           finish consuming.  Clearing first (not after) ensures we never
           miss a wake when bytes arrive mid-drain. */
        atomic_store_explicit(&term->wake_pending, false, memory_order_release);

        /* Drain at most SOL_TERM_DRAIN_BYTES_PER_FRAME bytes per call.
           This bounds per-frame VT-parse work so flood-output programs
           (yes, cmatrix) cannot starve the rest of the UI.  Any bytes left
           in the ring are consumed in subsequent frames; the reader will
           schedule another wake if needed. */
        char local[SOL_TERM_DRAIN_BYTES_PER_FRAME];
        size_t n = 0;
        pthread_mutex_lock(&term->output_mutex);
        while (n < SOL_TERM_DRAIN_BYTES_PER_FRAME &&
               term->output_tail != term->output_head) {
            local[n++] = term->output_ring[term->output_tail];
            term->output_tail = (term->output_tail + 1) % SOL_TERM_OUTPUT_RING_SIZE;
        }
        const bool ring_has_more = (term->output_tail != term->output_head);
        pthread_mutex_unlock(&term->output_mutex);

        if (n > 0) {
            term->dirty = false;
            term->sync_output_closed = false;
            for (size_t j = 0; j < n; ++j)
                vt_utf8_feed(term, &term->utf8_state, (uint8_t)local[j]);

            /* Grid state is always kept current, but while synchronized
               output (mode 2026) is active the UI is not told to repaint —
               this is what lets a full-screen TUI redraw land atomically
               instead of tearing mid-frame. A runaway app that never sends
               the closing ?2026l is bounded by a timeout so output is never
               permanently withheld. Closing the mode (or a fresh drain after
               timeout) always flushes once. */
            const bool sync_timed_out = term->mode_sync_output &&
                (sol_platform_now_monotonic_ns() - term->sync_output_start_ns) >
                    SOL_TERM_SYNC_OUTPUT_TIMEOUT_NS;
            if (!term->mode_sync_output || sync_timed_out || term->sync_output_closed) {
                any_dirty = true;
            }
        }

        /* If we hit the per-frame cap and bytes remain, schedule another
           wake so the next frame picks up where we left off.  The reader
           thread won't do this for us because wake_pending is now false
           but no new read() has fired yet. */
        if (ring_has_more) {
            atomic_store_explicit(&term->wake_pending, true, memory_order_release);
            ca_instance_wake();
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
            /* New rows beyond old rows: allocate. On OOM, clip growth
               to the last successfully allocated row rather than
               claiming rows still hold a NULL cells buffer below
               (term->rows is set to `rows` unconditionally further
               down; every write path assumes cells is non-NULL for
               any row < term->rows). */
            if (!term_line_alloc(&term->screen[r], cols)) {
                rows = r;
                break;
            }
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

    /* Notify PTY / SSH channel. */
#if !defined(_WIN32)
    if (term->is_ssh) {
        if (term->ssh_channel) {
            /* Best-effort: a resize request that fails (e.g. the
               channel is mid-teardown) is not worth surfacing as an
               error — the next successful resize corrects the remote
               PTY size, same as a dropped TIOCSWINSZ would be. */
            (void)libssh2_channel_request_pty_size(term->ssh_channel, cols, rows);
        }
    } else if (term->master_fd >= 0) {
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
    if (term->is_ssh) {
        if (!term->ssh_channel) return;
        size_t off = 0;
        while (off < len) {
            ssize_t n = libssh2_channel_write_ex(term->ssh_channel, 0,
                                                 data + off, len - off);
            if (n == LIBSSH2_ERROR_EAGAIN) continue;   /* blocking session; rare */
            if (n < 0) {
                /* Any other libssh2 error (channel closed remotely,
                   session torn down, etc.) means this session is done —
                   matches the local-PTY path's EIO/EBADF/EPIPE handling
                   above: mark dead rather than looping or crashing. */
                term->is_alive = false;
                break;
            }
            off += (size_t)n;
        }
        return;
    }
    if (term->master_fd < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(term->master_fd, data + off, len - off);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (errno == EIO || errno == EBADF || errno == EPIPE) {
                term->is_alive = false;
            }
            break;
        }
        off += (size_t)n;
    }
#else
    DWORD written;
    WriteFile(term->hPipeIn, data, (DWORD)len, &written, NULL);
#endif
}

void sol_terminal_paste(SolTerminal *term, const char *data, size_t len)
{
    if (!term || !data || len == 0 || !term->is_alive) return;
    if (term->mode_bracketed_paste) {
        static const char bracket_open[]  = "\033[200~";
        static const char bracket_close[] = "\033[201~";
        sol_terminal_send_text(term, bracket_open,  sizeof(bracket_open)  - 1u);
        sol_terminal_send_text(term, data, len);
        sol_terminal_send_text(term, bracket_close, sizeof(bracket_close) - 1u);
    } else {
        sol_terminal_send_text(term, data, len);
    }
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

static bool term_ctrl_code(uint32_t key, uint8_t mods, char *out)
{
    if (!(mods & SOL_MOD_CTRL) || !out) return false;

    if (key >= 'a' && key <= 'z') key = (uint32_t)(key - ('a' - 'A'));
    if (key >= 'A' && key <= 'Z') {
        *out = (char)(key & 0x1Fu);
        return true;
    }

    switch (key) {
    case ' ':
    case '2': *out = 0x00; return true;
    case '[':
    case '3': *out = 0x1B; return true;
    case '\\':
    case '4': *out = 0x1C; return true;
    case ']':
    case '5': *out = 0x1D; return true;
    case '^':
    case '6': *out = 0x1E; return true;
    case '_':
    case '7': *out = 0x1F; return true;
    case '?':
    case '/':
    case '8':
    case SOL_KEY_BACKSPACE:
        *out = 0x7F;
        return true;
    default:
        return false;
    }
}

static char term_ascii_for_key(uint32_t key, uint8_t mods)
{
    const bool shift = (mods & SOL_MOD_SHIFT) != 0u;

    if (key >= 'a' && key <= 'z') key = (uint32_t)(key - ('a' - 'A'));
    if (key >= 'A' && key <= 'Z') {
        return (char)(shift ? key : (key + ('a' - 'A')));
    }
    if (key >= '0' && key <= '9') {
        static const char shifted_digits[] = ")!@#$%^&*(";
        return shift ? shifted_digits[key - '0'] : (char)key;
    }

    switch (key) {
    case '`': return shift ? '~' : '`';
    case '-': return shift ? '_' : '-';
    case '=': return shift ? '+' : '=';
    case '[': return shift ? '{' : '[';
    case ']': return shift ? '}' : ']';
    case '\\': return shift ? '|' : '\\';
    case ';': return shift ? ':' : ';';
    case '\'': return shift ? '"' : '\'';
    case ',': return shift ? '<' : ',';
    case '.': return shift ? '>' : '.';
    case '/': return shift ? '?' : '/';
    case ' ': return ' ';
    default:
        if (key >= 32u && key <= 126u) return (char)key;
        return '\0';
    }
}

void sol_terminal_send_key(SolTerminal *term, uint32_t key, uint8_t mods)
{
    if (!term || !term->is_alive) return;

    char buf[32];
    int  len = 0;

    switch (key) {
    case SOL_KEY_LEFT_SHIFT:
    case SOL_KEY_RIGHT_SHIFT:
    case SOL_KEY_LEFT_CTRL:
    case SOL_KEY_RIGHT_CTRL:
    case SOL_KEY_LEFT_ALT:
    case SOL_KEY_RIGHT_ALT:
    case SOL_KEY_LEFT_SUPER:
    case SOL_KEY_RIGHT_SUPER:
        return;
    default:
        break;
    }

    /* Kitty keyboard protocol: disambiguate escape codes (bit 0). Legacy
       encoding collapses distinct keys onto the same bytes an application
       cannot tell apart — plain ESC vs Ctrl+[ vs Alt+Escape, Enter vs
       Ctrl+M, Tab vs Ctrl+I, Backspace vs Ctrl+H/Ctrl+Backspace. An app
       that opted into bit 0 gets each of these as a distinct CSI-u report
       instead. Only keys with a real ambiguity are covered; unambiguous
       keys (arrows, function keys, plain printable chars) keep their
       normal encoding even with disambiguation active, matching the Kitty
       spec's "only when necessary" guidance. */
    if (term->kitty_flags_depth > 0 &&
        (term->kitty_flags_stack[term->kitty_flags_depth - 1] & 0x1u)) {
        int  csi_code = 0;
        bool covered  = true;
        switch (key) {
        case SOL_KEY_ESCAPE:    csi_code = 27;  break;
        case SOL_KEY_ENTER:     csi_code = 13;  break;
        case SOL_KEY_TAB:       csi_code = 9;   break;
        case SOL_KEY_BACKSPACE: csi_code = 127; break;
        default: covered = false; break;
        }
        if (covered) {
            const int modn = term_modifier_number(mods);
            if (modn > 1) {
                len = snprintf(buf, sizeof(buf), "\033[%d;%du", csi_code, modn);
            } else {
                len = snprintf(buf, sizeof(buf), "\033[%du", csi_code);
            }
            sol_terminal_send_text(term, buf, (size_t)len);
            return;
        }
    }

    /* Ctrl and Ctrl+Alt chords map to C0 controls.  Alt prefixes ESC. */
    char ctrl = 0;
    if (term_ctrl_code(key, mods, &ctrl)) {
        if (mods & SOL_MOD_ALT) {
            buf[len++] = '\033';
        }
        buf[len++] = ctrl;
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

    struct { uint32_t key; const char *normal; int vt_num; } funcs[] = {
        { SOL_KEY_F1,  "\033OP", 11 },
        { SOL_KEY_F2,  "\033OQ", 12 },
        { SOL_KEY_F3,  "\033OR", 13 },
        { SOL_KEY_F4,  "\033OS", 14 },
        { SOL_KEY_F5,  "\033[15~", 15 },
        { SOL_KEY_F6,  "\033[17~", 17 },
        { SOL_KEY_F7,  "\033[18~", 18 },
        { SOL_KEY_F8,  "\033[19~", 19 },
        { SOL_KEY_F9,  "\033[20~", 20 },
        { SOL_KEY_F10, "\033[21~", 21 },
        { SOL_KEY_F11, "\033[23~", 23 },
        { SOL_KEY_F12, "\033[24~", 24 },
    };
    for (size_t i = 0; i < sizeof(funcs)/sizeof(funcs[0]); ++i) {
        if (key == funcs[i].key) {
            if (modn > 1) {
                if (key >= SOL_KEY_F1 && key <= SOL_KEY_F4) {
                    const char final = (char)('P' + (key - SOL_KEY_F1));
                    len = snprintf(buf, sizeof(buf), "\033[1;%d%c", modn, final);
                } else {
                    len = snprintf(buf, sizeof(buf), "\033[%d;%d~",
                                   funcs[i].vt_num, modn);
                }
            } else {
                len = snprintf(buf, sizeof(buf), "%s", funcs[i].normal);
            }
            sol_terminal_send_text(term, buf, (size_t)len);
            return;
        }
    }

    switch (key) {
    case SOL_KEY_ESCAPE:
        if (mods & SOL_MOD_ALT) {
            buf[0] = '\033'; buf[1] = '\033'; len = 2;
        } else {
            buf[0] = '\033'; len = 1;
        }
        break;
    case SOL_KEY_BACKSPACE:
        if (mods & SOL_MOD_ALT) {
            buf[0] = '\033'; buf[1] = 0x7F; len = 2;
        } else {
            buf[0] = 0x7F; len = 1;
        }
        break;
    case SOL_KEY_ENTER:
        if (mods & SOL_MOD_ALT) {
            buf[0] = '\033'; buf[1] = '\r'; len = 2;
        } else {
            buf[0] = '\r'; len = 1;
        }
        break;
    case SOL_KEY_TAB:
        if (mods & SOL_MOD_SHIFT) {
            buf[0] = '\033'; buf[1] = '['; buf[2] = 'Z'; len = 3;
        } else if (mods & SOL_MOD_ALT) {
            buf[0] = '\033'; buf[1] = '\t'; len = 2;
        } else {
            buf[0] = '\t'; len = 1;
        }
        break;
    default: break;
    }

    if (len == 0 && !(mods & (SOL_MOD_CTRL | SOL_MOD_SUPER))) {
        const char ch = term_ascii_for_key(key, mods);
        if (ch != '\0') {
            if (mods & SOL_MOD_ALT) {
                buf[0] = '\033';
                buf[1] = ch;
                len = 2;
            } else {
                buf[0] = ch;
                len = 1;
            }
        }
    }

    if (len > 0) {
        sol_terminal_send_text(term, buf, (size_t)len);
    }
}

bool sol_terminal_wants_mouse(const SolTerminal *term)
{
    return term && (term->mode_mouse_btn || term->mode_mouse_any);
}

void sol_terminal_send_mouse(SolTerminal *term, int col, int row,
                             int button, SolTermMouseAction action,
                             uint8_t mods)
{
    if (!term || !term->is_alive) return;
    if (!term->mode_mouse_btn && !term->mode_mouse_any) return;
    if (action == SOL_TERM_MOUSE_MOVE && !term->mode_mouse_any) return;
    if (col < 0 || row < 0) return;

    /* SGR extended encoding (mode 1006) is what every modern full-screen
       TUI expects; it is the only encoding sol_terminal implements since
       the legacy X10/UTF-8 encodings cap coordinates at 223 and are not
       what current applications request. If the app enabled tracking but
       not SGR, coordinates still fit unmodified/most terminals treat SGR
       as the safe universal choice — apps that need legacy encoding are
       effectively unsupported here, matching many modern terminals. */
    int cb;
    switch (action) {
    case SOL_TERM_MOUSE_WHEEL_UP:   cb = 64; break;
    case SOL_TERM_MOUSE_WHEEL_DOWN: cb = 65; break;
    default:
        cb = button & 0x3;
        if (action == SOL_TERM_MOUSE_MOVE) cb |= 32; /* motion flag */
        break;
    }
    if (mods & SOL_MOD_SHIFT) cb |= 4;
    if (mods & SOL_MOD_ALT)   cb |= 8;
    if (mods & SOL_MOD_CTRL)  cb |= 16;

    const char final = (action == SOL_TERM_MOUSE_RELEASE) ? 'm' : 'M';
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
                        cb, col + 1, row + 1, final);
    if (len > 0) {
        sol_terminal_send_text(term, buf, (size_t)len);
    }
}

void sol_terminal_kill(SolTerminal *term)
{
    if (!term) return;
#if !defined(_WIN32)
    if (term->child_pid > 0) {
        kill(term->child_pid, SIGKILL);
    }
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
