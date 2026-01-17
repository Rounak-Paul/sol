/*
 * Sol IDE - Terminal Emulator Implementation
 * Cross-platform PTY support for integrated terminal
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    /* Windows ConPTY */
    typedef struct {
        HPCON pty;
        HANDLE pipe_in;
        HANDLE pipe_out;
        HANDLE process;
        HANDLE thread;
    } pty_handle;
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <sys/ioctl.h>
    #include <signal.h>
    #if defined(__linux__) || defined(__FreeBSD__)
        #include <pty.h>
    #elif defined(__APPLE__)
        #include <util.h>
    #endif
    typedef struct {
        int master_fd;
        pid_t child_pid;
    } pty_handle;
#endif

#define TERM_SCROLLBACK 10000
#define TERM_MAX_PARAMS 16

/* Terminal character attributes */
typedef struct {
    uint32_t fg;
    uint32_t bg;
    bool bold;
    bool italic;
    bool underline;
    bool inverse;
} term_attr;

/* Terminal cell */
typedef struct {
    uint32_t ch;
    term_attr attr;
} term_cell;

/* Terminal state */
struct sol_terminal {
    /* PTY handle */
    pty_handle pty;
    bool pty_active;
    
    /* Screen buffer */
    term_cell* screen;
    term_cell* alt_screen;  /* Alternate screen buffer */
    bool use_alt_screen;
    
    int cols;
    int rows;
    
    /* Cursor */
    int cursor_x;
    int cursor_y;
    bool cursor_visible;
    
    /* Scrollback buffer */
    term_cell* scrollback;
    int scrollback_lines;
    int scroll_offset;
    
    /* Current attributes */
    term_attr current_attr;
    
    /* Parser state for escape sequences */
    int parse_state;
    char parse_buf[256];
    int parse_len;
    int params[TERM_MAX_PARAMS];
    int param_count;
    
    /* Scroll region */
    int scroll_top;
    int scroll_bottom;
    
    /* Modes */
    bool mode_insert;
    bool mode_autowrap;
    bool mode_origin;
    
    /* Title */
    char title[256];
    
    /* I/O buffer */
    char read_buf[4096];
    int read_len;
};

/* Forward declaration */
static void sol_terminal_close(sol_terminal* term);

/* Default colors (ANSI 256) */
static uint32_t ansi_colors[256] = {
    /* Standard colors 0-7 */
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00,
    0x0000CD, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    /* Bright colors 8-15 */
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    /* 216 color cube 16-231 + grayscale 232-255 initialized elsewhere */
};

static void init_color_table(void) {
    /* Initialize 216 color cube (16-231) */
    int i = 16;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                ansi_colors[i++] = ((r ? 55 + r * 40 : 0) << 16) |
                                   ((g ? 55 + g * 40 : 0) << 8) |
                                   (b ? 55 + b * 40 : 0);
            }
        }
    }
    /* Grayscale 232-255 */
    for (int j = 0; j < 24; j++) {
        int v = 8 + j * 10;
        ansi_colors[232 + j] = (v << 16) | (v << 8) | v;
    }
}

/* Create terminal */
sol_terminal* sol_terminal_create(int cols, int rows) {
    static bool colors_initialized = false;
    if (!colors_initialized) {
        init_color_table();
        colors_initialized = true;
    }
    
    sol_terminal* term = (sol_terminal*)calloc(1, sizeof(sol_terminal));
    if (!term) return NULL;
    
    term->cols = cols;
    term->rows = rows;
    term->cursor_visible = true;
    term->mode_autowrap = true;
    term->scroll_bottom = rows - 1;
    
    /* Default attributes */
    term->current_attr.fg = 7;  /* White */
    term->current_attr.bg = 0;  /* Black */
    
    /* Allocate screen buffer */
    size_t screen_size = (size_t)(cols * rows) * sizeof(term_cell);
    term->screen = (term_cell*)calloc(1, screen_size);
    term->alt_screen = (term_cell*)calloc(1, screen_size);
    
    /* Allocate scrollback */
    term->scrollback = (term_cell*)calloc(TERM_SCROLLBACK * (size_t)cols, sizeof(term_cell));
    
    if (!term->screen || !term->alt_screen || !term->scrollback) {
        free(term->screen);
        free(term->alt_screen);
        free(term->scrollback);
        free(term);
        return NULL;
    }
    
    /* Clear screen with spaces */
    for (int i = 0; i < cols * rows; i++) {
        term->screen[i].ch = ' ';
        term->screen[i].attr = term->current_attr;
        term->alt_screen[i].ch = ' ';
        term->alt_screen[i].attr = term->current_attr;
    }
    
    return term;
}

void sol_terminal_destroy(sol_terminal* term) {
    if (!term) return;
    
    sol_terminal_close(term);
    
    free(term->screen);
    free(term->alt_screen);
    free(term->scrollback);
    free(term);
}

/* Start shell process */
sol_result sol_terminal_spawn(sol_terminal* term, const char* shell) {
    if (!term) return SOL_ERROR_INVALID_ARG;
    
    const char* default_shell;
    
#ifdef _WIN32
    default_shell = shell ? shell : "cmd.exe";
    
    /* Windows ConPTY implementation */
    HRESULT hr;
    COORD size = { (SHORT)term->cols, (SHORT)term->rows };
    
    /* Create pipes */
    HANDLE pipeIn_read, pipeIn_write;
    HANDLE pipeOut_read, pipeOut_write;
    
    if (!CreatePipe(&pipeIn_read, &pipeIn_write, NULL, 0)) return SOL_ERROR_IO;
    if (!CreatePipe(&pipeOut_read, &pipeOut_write, NULL, 0)) {
        CloseHandle(pipeIn_read);
        CloseHandle(pipeIn_write);
        return SOL_ERROR_IO;
    }
    
    /* Create pseudo console */
    hr = CreatePseudoConsole(size, pipeIn_read, pipeOut_write, 0, &term->pty.pty);
    if (FAILED(hr)) {
        CloseHandle(pipeIn_read);
        CloseHandle(pipeIn_write);
        CloseHandle(pipeOut_read);
        CloseHandle(pipeOut_write);
        return SOL_ERROR_IO;
    }
    
    /* Set up startup info */
    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    
    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
    si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(attrListSize);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              term->pty.pty, sizeof(HPCON), NULL, NULL);
    
    /* Create process */
    PROCESS_INFORMATION pi = {0};
    wchar_t cmdline[256];
    mbstowcs(cmdline, default_shell, 256);
    
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        ClosePseudoConsole(term->pty.pty);
        free(si.lpAttributeList);
        CloseHandle(pipeIn_read);
        CloseHandle(pipeIn_write);
        CloseHandle(pipeOut_read);
        CloseHandle(pipeOut_write);
        return SOL_ERROR_IO;
    }
    
    term->pty.pipe_in = pipeIn_write;
    term->pty.pipe_out = pipeOut_read;
    term->pty.process = pi.hProcess;
    term->pty.thread = pi.hThread;
    
    CloseHandle(pipeIn_read);
    CloseHandle(pipeOut_write);
    free(si.lpAttributeList);
    
#else
    default_shell = shell ? shell : getenv("SHELL");
    if (!default_shell) default_shell = "/bin/sh";
    
    struct winsize ws;
    ws.ws_col = (unsigned short)term->cols;
    ws.ws_row = (unsigned short)term->rows;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    
    term->pty.child_pid = forkpty(&term->pty.master_fd, NULL, NULL, &ws);
    
    if (term->pty.child_pid < 0) {
        return SOL_ERROR_IO;
    }
    
    if (term->pty.child_pid == 0) {
        /* Child process */
        setenv("TERM", "xterm-256color", 1);
        execl(default_shell, default_shell, (char*)NULL);
        _exit(127);
    }
    
    /* Parent - set non-blocking */
    int flags = fcntl(term->pty.master_fd, F_GETFL);
    fcntl(term->pty.master_fd, F_SETFL, flags | O_NONBLOCK);
#endif
    
    term->pty_active = true;
    return SOL_OK;
}

static void sol_terminal_close(sol_terminal* term) {
    if (!term || !term->pty_active) return;
    
#ifdef _WIN32
    ClosePseudoConsole(term->pty.pty);
    CloseHandle(term->pty.pipe_in);
    CloseHandle(term->pty.pipe_out);
    TerminateProcess(term->pty.process, 0);
    CloseHandle(term->pty.process);
    CloseHandle(term->pty.thread);
#else
    close(term->pty.master_fd);
    kill(term->pty.child_pid, SIGTERM);
    waitpid(term->pty.child_pid, NULL, WNOHANG);
#endif
    
    term->pty_active = false;
}

/* Resize terminal */
void sol_terminal_resize(sol_terminal* term, int cols, int rows) {
    if (!term || cols <= 0 || rows <= 0) return;
    if (cols == term->cols && rows == term->rows) return;
    
    /* Allocate new buffers */
    size_t new_size = (size_t)(cols * rows) * sizeof(term_cell);
    term_cell* new_screen = (term_cell*)calloc(1, new_size);
    term_cell* new_alt = (term_cell*)calloc(1, new_size);
    
    if (!new_screen || !new_alt) {
        free(new_screen);
        free(new_alt);
        return;
    }
    
    /* Initialize with spaces */
    for (int i = 0; i < cols * rows; i++) {
        new_screen[i].ch = ' ';
        new_screen[i].attr = term->current_attr;
        new_alt[i].ch = ' ';
        new_alt[i].attr = term->current_attr;
    }
    
    /* Copy old content */
    int copy_rows = rows < term->rows ? rows : term->rows;
    int copy_cols = cols < term->cols ? cols : term->cols;
    
    for (int y = 0; y < copy_rows; y++) {
        for (int x = 0; x < copy_cols; x++) {
            new_screen[y * cols + x] = term->screen[y * term->cols + x];
            new_alt[y * cols + x] = term->alt_screen[y * term->cols + x];
        }
    }
    
    free(term->screen);
    free(term->alt_screen);
    term->screen = new_screen;
    term->alt_screen = new_alt;
    
    term->cols = cols;
    term->rows = rows;
    term->scroll_bottom = rows - 1;
    
    /* Adjust cursor */
    if (term->cursor_x >= cols) term->cursor_x = cols - 1;
    if (term->cursor_y >= rows) term->cursor_y = rows - 1;
    
    /* Notify PTY of resize */
    if (term->pty_active) {
#ifdef _WIN32
        COORD size = { (SHORT)cols, (SHORT)rows };
        ResizePseudoConsole(term->pty.pty, size);
#else
        struct winsize ws;
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(term->pty.master_fd, TIOCSWINSZ, &ws);
#endif
    }
}

/* Write to terminal (input from user) */
void sol_terminal_write(sol_terminal* term, const char* data, size_t len) {
    if (!term || !term->pty_active || !data || len == 0) return;
    
#ifdef _WIN32
    DWORD written;
    WriteFile(term->pty.pipe_in, data, (DWORD)len, &written, NULL);
#else
    write(term->pty.master_fd, data, len);
#endif
}

/* Scroll screen up */
static void scroll_up(sol_terminal* term, int lines) {
    if (lines <= 0) return;
    
    term_cell* buf = term->use_alt_screen ? term->alt_screen : term->screen;
    
    int top = term->scroll_top;
    int bottom = term->scroll_bottom;
    int region_height = bottom - top + 1;
    
    if (lines >= region_height) {
        /* Clear entire region */
        for (int y = top; y <= bottom; y++) {
            for (int x = 0; x < term->cols; x++) {
                buf[y * term->cols + x].ch = ' ';
                buf[y * term->cols + x].attr = term->current_attr;
            }
        }
        return;
    }
    
    /* Add to scrollback (only for main screen) */
    if (!term->use_alt_screen && top == 0 && term->scrollback) {
        /* TODO: implement scrollback storage */
    }
    
    /* Scroll region */
    for (int y = top; y <= bottom - lines; y++) {
        memcpy(&buf[y * term->cols], 
               &buf[(y + lines) * term->cols],
               (size_t)term->cols * sizeof(term_cell));
    }
    
    /* Clear new lines at bottom */
    for (int y = bottom - lines + 1; y <= bottom; y++) {
        for (int x = 0; x < term->cols; x++) {
            buf[y * term->cols + x].ch = ' ';
            buf[y * term->cols + x].attr = term->current_attr;
        }
    }
}

/* Process SGR (Select Graphic Rendition) escape */
static void process_sgr(sol_terminal* term) {
    if (term->param_count == 0) {
        term->params[0] = 0;
        term->param_count = 1;
    }
    
    for (int i = 0; i < term->param_count; i++) {
        int p = term->params[i];
        
        switch (p) {
            case 0:  /* Reset */
                term->current_attr.fg = 7;
                term->current_attr.bg = 0;
                term->current_attr.bold = false;
                term->current_attr.italic = false;
                term->current_attr.underline = false;
                term->current_attr.inverse = false;
                break;
            case 1: term->current_attr.bold = true; break;
            case 3: term->current_attr.italic = true; break;
            case 4: term->current_attr.underline = true; break;
            case 7: term->current_attr.inverse = true; break;
            case 22: term->current_attr.bold = false; break;
            case 23: term->current_attr.italic = false; break;
            case 24: term->current_attr.underline = false; break;
            case 27: term->current_attr.inverse = false; break;
            
            /* Foreground colors */
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                term->current_attr.fg = (uint32_t)(p - 30);
                break;
            case 39: term->current_attr.fg = 7; break;  /* Default */
            
            /* Background colors */
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                term->current_attr.bg = (uint32_t)(p - 40);
                break;
            case 49: term->current_attr.bg = 0; break;  /* Default */
            
            /* Bright foreground */
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                term->current_attr.fg = (uint32_t)(p - 90 + 8);
                break;
            
            /* Bright background */
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                term->current_attr.bg = (uint32_t)(p - 100 + 8);
                break;
            
            /* 256 color / 24-bit color */
            case 38:
                if (i + 2 < term->param_count && term->params[i + 1] == 5) {
                    term->current_attr.fg = (uint32_t)term->params[i + 2];
                    i += 2;
                } else if (i + 4 < term->param_count && term->params[i + 1] == 2) {
                    term->current_attr.fg = 0x1000000 |  /* Flag for RGB */
                        ((uint32_t)term->params[i + 2] << 16) |
                        ((uint32_t)term->params[i + 3] << 8) |
                        (uint32_t)term->params[i + 4];
                    i += 4;
                }
                break;
            case 48:
                if (i + 2 < term->param_count && term->params[i + 1] == 5) {
                    term->current_attr.bg = (uint32_t)term->params[i + 2];
                    i += 2;
                } else if (i + 4 < term->param_count && term->params[i + 1] == 2) {
                    term->current_attr.bg = 0x1000000 |
                        ((uint32_t)term->params[i + 2] << 16) |
                        ((uint32_t)term->params[i + 3] << 8) |
                        (uint32_t)term->params[i + 4];
                    i += 4;
                }
                break;
        }
    }
}

/* Process CSI (Control Sequence Introducer) escape */
static void process_csi(sol_terminal* term, char final) {
    term_cell* buf = term->use_alt_screen ? term->alt_screen : term->screen;
    
    int n = term->param_count > 0 ? term->params[0] : 1;
    int m = term->param_count > 1 ? term->params[1] : 1;
    
    switch (final) {
        case 'A':  /* Cursor up */
            term->cursor_y -= n;
            if (term->cursor_y < 0) term->cursor_y = 0;
            break;
            
        case 'B':  /* Cursor down */
            term->cursor_y += n;
            if (term->cursor_y >= term->rows) term->cursor_y = term->rows - 1;
            break;
            
        case 'C':  /* Cursor forward */
            term->cursor_x += n;
            if (term->cursor_x >= term->cols) term->cursor_x = term->cols - 1;
            break;
            
        case 'D':  /* Cursor back */
            term->cursor_x -= n;
            if (term->cursor_x < 0) term->cursor_x = 0;
            break;
            
        case 'H':  /* Cursor position */
        case 'f':
            term->cursor_y = (term->param_count > 0 ? term->params[0] : 1) - 1;
            term->cursor_x = (term->param_count > 1 ? term->params[1] : 1) - 1;
            if (term->cursor_x < 0) term->cursor_x = 0;
            if (term->cursor_y < 0) term->cursor_y = 0;
            if (term->cursor_x >= term->cols) term->cursor_x = term->cols - 1;
            if (term->cursor_y >= term->rows) term->cursor_y = term->rows - 1;
            break;
            
        case 'J':  /* Erase in display */
            n = term->param_count > 0 ? term->params[0] : 0;
            switch (n) {
                case 0:  /* Cursor to end */
                    for (int i = term->cursor_y * term->cols + term->cursor_x;
                         i < term->cols * term->rows; i++) {
                        buf[i].ch = ' ';
                        buf[i].attr = term->current_attr;
                    }
                    break;
                case 1:  /* Start to cursor */
                    for (int i = 0; i <= term->cursor_y * term->cols + term->cursor_x; i++) {
                        buf[i].ch = ' ';
                        buf[i].attr = term->current_attr;
                    }
                    break;
                case 2:  /* Entire screen */
                case 3:
                    for (int i = 0; i < term->cols * term->rows; i++) {
                        buf[i].ch = ' ';
                        buf[i].attr = term->current_attr;
                    }
                    break;
            }
            break;
            
        case 'K':  /* Erase in line */
            n = term->param_count > 0 ? term->params[0] : 0;
            switch (n) {
                case 0:  /* Cursor to end */
                    for (int x = term->cursor_x; x < term->cols; x++) {
                        buf[term->cursor_y * term->cols + x].ch = ' ';
                        buf[term->cursor_y * term->cols + x].attr = term->current_attr;
                    }
                    break;
                case 1:  /* Start to cursor */
                    for (int x = 0; x <= term->cursor_x; x++) {
                        buf[term->cursor_y * term->cols + x].ch = ' ';
                        buf[term->cursor_y * term->cols + x].attr = term->current_attr;
                    }
                    break;
                case 2:  /* Entire line */
                    for (int x = 0; x < term->cols; x++) {
                        buf[term->cursor_y * term->cols + x].ch = ' ';
                        buf[term->cursor_y * term->cols + x].attr = term->current_attr;
                    }
                    break;
            }
            break;
            
        case 'm':  /* SGR */
            process_sgr(term);
            break;
            
        case 'r':  /* Set scroll region */
            term->scroll_top = (term->param_count > 0 ? term->params[0] : 1) - 1;
            term->scroll_bottom = (term->param_count > 1 ? term->params[1] : term->rows) - 1;
            if (term->scroll_top < 0) term->scroll_top = 0;
            if (term->scroll_bottom >= term->rows) term->scroll_bottom = term->rows - 1;
            term->cursor_x = 0;
            term->cursor_y = 0;
            break;
            
        case 'h':  /* Set mode */
        case 'l':  /* Reset mode */
            if (term->parse_len > 0 && term->parse_buf[0] == '?') {
                /* DEC private modes */
                bool set = (final == 'h');
                switch (n) {
                    case 1: /* DECCKM - Cursor keys */
                        break;
                    case 25: /* DECTCEM - Show cursor */
                        term->cursor_visible = set;
                        break;
                    case 1049: /* Alternate screen buffer */
                        if (set && !term->use_alt_screen) {
                            term->use_alt_screen = true;
                            term->cursor_x = 0;
                            term->cursor_y = 0;
                        } else if (!set && term->use_alt_screen) {
                            term->use_alt_screen = false;
                        }
                        break;
                }
            }
            break;
            
        default:
            break;
    }
}

/* Process escape sequence */
static void process_escape(sol_terminal* term, char c) {
    switch (term->parse_state) {
        case 1:  /* After ESC */
            switch (c) {
                case '[':
                    term->parse_state = 2;  /* CSI */
                    term->parse_len = 0;
                    term->param_count = 0;
                    memset(term->params, 0, sizeof(term->params));
                    break;
                case ']':
                    term->parse_state = 3;  /* OSC */
                    term->parse_len = 0;
                    break;
                case '(':
                case ')':
                    term->parse_state = 4;  /* Charset */
                    break;
                case '7':  /* Save cursor */
                    /* TODO */
                    term->parse_state = 0;
                    break;
                case '8':  /* Restore cursor */
                    /* TODO */
                    term->parse_state = 0;
                    break;
                case 'M':  /* Reverse index */
                    if (term->cursor_y > term->scroll_top) {
                        term->cursor_y--;
                    }
                    /* TODO: scroll down if at top */
                    term->parse_state = 0;
                    break;
                default:
                    term->parse_state = 0;
                    break;
            }
            break;
            
        case 2:  /* CSI */
            if (c >= '0' && c <= '9') {
                if (term->param_count == 0) term->param_count = 1;
                term->params[term->param_count - 1] *= 10;
                term->params[term->param_count - 1] += (c - '0');
            } else if (c == ';') {
                if (term->param_count < TERM_MAX_PARAMS) {
                    term->param_count++;
                }
            } else if (c == '?' || c == '>') {
                if (term->parse_len < (int)sizeof(term->parse_buf) - 1) {
                    term->parse_buf[term->parse_len++] = c;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                process_csi(term, c);
                term->parse_state = 0;
            } else {
                term->parse_state = 0;
            }
            break;
            
        case 3:  /* OSC */
            if (c == '\007' || c == '\033') {
                /* End of OSC */
                term->parse_buf[term->parse_len] = '\0';
                /* Parse OSC (e.g., title setting) */
                if (term->parse_len > 2 && term->parse_buf[1] == ';') {
                    if (term->parse_buf[0] == '0' || term->parse_buf[0] == '2') {
                        strncpy(term->title, &term->parse_buf[2], sizeof(term->title) - 1);
                    }
                }
                term->parse_state = (c == '\033') ? 1 : 0;
            } else if (term->parse_len < (int)sizeof(term->parse_buf) - 1) {
                term->parse_buf[term->parse_len++] = c;
            }
            break;
            
        case 4:  /* Charset - ignore */
            term->parse_state = 0;
            break;
            
        default:
            term->parse_state = 0;
            break;
    }
}

/* Put character at cursor */
static void put_char(sol_terminal* term, uint32_t ch) {
    term_cell* buf = term->use_alt_screen ? term->alt_screen : term->screen;
    
    if (term->cursor_x >= term->cols) {
        if (term->mode_autowrap) {
            term->cursor_x = 0;
            term->cursor_y++;
            if (term->cursor_y > term->scroll_bottom) {
                term->cursor_y = term->scroll_bottom;
                scroll_up(term, 1);
            }
        } else {
            term->cursor_x = term->cols - 1;
        }
    }
    
    int idx = term->cursor_y * term->cols + term->cursor_x;
    buf[idx].ch = ch;
    buf[idx].attr = term->current_attr;
    term->cursor_x++;
}

/* Read from PTY and process output */
void sol_terminal_update(sol_terminal* term) {
    if (!term || !term->pty_active) return;
    
    char buf[4096];
    ssize_t nread;
    
#ifdef _WIN32
    DWORD available = 0;
    if (!PeekNamedPipe(term->pty.pipe_out, NULL, 0, NULL, &available, NULL)) {
        return;
    }
    if (available == 0) return;
    
    DWORD bytesRead;
    if (!ReadFile(term->pty.pipe_out, buf, sizeof(buf), &bytesRead, NULL)) {
        return;
    }
    nread = (ssize_t)bytesRead;
#else
    nread = read(term->pty.master_fd, buf, sizeof(buf));
    if (nread <= 0) return;
#endif
    
    /* Process output */
    for (ssize_t i = 0; i < nread; i++) {
        unsigned char c = (unsigned char)buf[i];
        
        if (term->parse_state > 0) {
            process_escape(term, (char)c);
            continue;
        }
        
        switch (c) {
            case '\033':  /* ESC */
                term->parse_state = 1;
                break;
            case '\r':  /* CR */
                term->cursor_x = 0;
                break;
            case '\n':  /* LF */
                term->cursor_y++;
                if (term->cursor_y > term->scroll_bottom) {
                    term->cursor_y = term->scroll_bottom;
                    scroll_up(term, 1);
                }
                break;
            case '\b':  /* BS */
                if (term->cursor_x > 0) term->cursor_x--;
                break;
            case '\t':  /* Tab */
                term->cursor_x = ((term->cursor_x / 8) + 1) * 8;
                if (term->cursor_x >= term->cols) term->cursor_x = term->cols - 1;
                break;
            case '\007':  /* Bell */
                /* TODO: beep or flash */
                break;
            default:
                if (c >= 32 || c >= 128) {
                    put_char(term, c);
                }
                break;
        }
    }
}

/* Get cell at position */
void sol_terminal_get_cell(sol_terminal* term, int x, int y, 
                           uint32_t* ch, uint32_t* fg, uint32_t* bg) {
    if (!term || x < 0 || x >= term->cols || y < 0 || y >= term->rows) {
        if (ch) *ch = ' ';
        if (fg) *fg = 0xFFFFFF;
        if (bg) *bg = 0x000000;
        return;
    }
    
    term_cell* buf = term->use_alt_screen ? term->alt_screen : term->screen;
    term_cell* cell = &buf[y * term->cols + x];
    
    if (ch) *ch = cell->ch;
    
    /* Convert color index to RGB */
    if (fg) {
        if (cell->attr.fg & 0x1000000) {
            *fg = cell->attr.fg & 0xFFFFFF;
        } else {
            *fg = ansi_colors[cell->attr.fg & 0xFF];
        }
        if (cell->attr.inverse) {
            /* Swap fg/bg handled by caller */
        }
    }
    
    if (bg) {
        if (cell->attr.bg & 0x1000000) {
            *bg = cell->attr.bg & 0xFFFFFF;
        } else {
            *bg = ansi_colors[cell->attr.bg & 0xFF];
        }
    }
}

bool sol_terminal_is_active(sol_terminal* term) {
    return term && term->pty_active;
}

void sol_terminal_get_cursor(sol_terminal* term, int* x, int* y) {
    if (x) *x = term ? term->cursor_x : 0;
    if (y) *y = term ? term->cursor_y : 0;
}

bool sol_terminal_cursor_visible(sol_terminal* term) {
    return term && term->cursor_visible;
}

const char* sol_terminal_get_title(sol_terminal* term) {
    return term ? term->title : "";
}
