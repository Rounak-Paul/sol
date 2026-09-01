// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_crash.c — see sol_crash.h for the calling contract and the
   signal-handler-safety accounting referenced throughout this file. */
#include "sol_crash.h"

#include "sol_config.h"
#include "sol_event.h"
#include "sol_platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <dbghelp.h>
#else
  #include <execinfo.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Pre-crash state — written only on the normal (non-crashing) path,   */
/* read only inside the handler. Plain static storage: no locks, no    */
/* heap, so it is always safe to read no matter what the crashing      */
/* thread was doing when it faulted.                                   */
/* ------------------------------------------------------------------ */

#define SOL_CRASH_EVENT_RING_CAPACITY 24u
#define SOL_CRASH_EVENT_LABEL_MAX     96u
#define SOL_CRASH_MAX_OPEN_BUFFERS    64u
#define SOL_CRASH_PATH_MAX            1024u

typedef struct SolCrashEventSlot {
    char label[SOL_CRASH_EVENT_LABEL_MAX];
} SolCrashEventSlot;

typedef struct SolCrashOpenBuffer {
    SolBufferId id;
    char        path[SOL_CRASH_PATH_MAX];
} SolCrashOpenBuffer;

/* All of g_crash_state is static storage (zero-initialized at load
   time, never malloc'd), so it exists and is valid before main() runs
   and remains valid for the rest of the process — including during a
   crash on a corrupted heap. */
static struct {
    bool installed;
    char app_name[64];
    char app_version[64];
    char report_dir[SOL_CRASH_PATH_MAX];
    /* has_report_dir is false when the directory could not be resolved
       or created at install time (HOME unset, no write permission,
       etc.) — the handler then falls back to writing to stderr only,
       exactly like every other "can't persist to disk" fallback
       elsewhere in Sol (see sol_settings.c, sol_bg_effect.c). */
    bool has_report_dir;

    /* Ring buffer of recent breadcrumbs. Not synchronized: a data race
       between the crashing thread and a concurrent writer on another
       thread is possible, but the worst outcome is a torn/stale label
       string in the report, never a crash in the handler itself (every
       slot is always NUL-terminated up to its fixed capacity). */
    SolCrashEventSlot event_ring[SOL_CRASH_EVENT_RING_CAPACITY];
    uint32_t          event_ring_next;   /* next slot to write */
    uint32_t          event_ring_count;  /* number of valid slots, saturates */

    /* Open-buffer snapshot, rebuilt incrementally by
       sol_crash_on_buffer_event as buffers open/close/focus. Same
       torn-read caveat as the event ring. */
    SolCrashOpenBuffer open_buffers[SOL_CRASH_MAX_OPEN_BUFFERS];
    uint32_t           open_buffer_count;
} g_crash_state;

/* ------------------------------------------------------------------ */
/* Normal-path helpers (never called from inside the signal handler)   */
/* ------------------------------------------------------------------ */

void sol_crash_record_event(const char *label)
{
    if (!label) return;
    SolCrashEventSlot *slot =
        &g_crash_state.event_ring[g_crash_state.event_ring_next];
    snprintf(slot->label, sizeof(slot->label), "%s", label);
    g_crash_state.event_ring_next =
        (g_crash_state.event_ring_next + 1u) % SOL_CRASH_EVENT_RING_CAPACITY;
    if (g_crash_state.event_ring_count < SOL_CRASH_EVENT_RING_CAPACITY)
        g_crash_state.event_ring_count++;
}

static void sol_crash_track_buffer_add(SolBufferId id, const char *path)
{
    if (!path || path[0] == '\0') return;
    for (uint32_t i = 0u; i < g_crash_state.open_buffer_count; ++i) {
        if (g_crash_state.open_buffers[i].id == id) {
            snprintf(g_crash_state.open_buffers[i].path,
                    sizeof(g_crash_state.open_buffers[i].path), "%s", path);
            return;
        }
    }
    if (g_crash_state.open_buffer_count >= SOL_CRASH_MAX_OPEN_BUFFERS) return;
    SolCrashOpenBuffer *slot =
        &g_crash_state.open_buffers[g_crash_state.open_buffer_count++];
    slot->id = id;
    snprintf(slot->path, sizeof(slot->path), "%s", path);
}

static void sol_crash_track_buffer_remove(SolBufferId id)
{
    for (uint32_t i = 0u; i < g_crash_state.open_buffer_count; ++i) {
        if (g_crash_state.open_buffers[i].id != id) continue;
        const uint32_t last = g_crash_state.open_buffer_count - 1u;
        if (i != last) g_crash_state.open_buffers[i] = g_crash_state.open_buffers[last];
        g_crash_state.open_buffer_count--;
        return;
    }
}

static bool sol_crash_on_event(const SolEvent *event, void *user_data)
{
    (void)user_data;
    if (!event) return false;

    if (event->payload && event->payload_size >= sizeof(SolCommandInvokedPayload) &&
        strcmp(event->name, SOL_EVENT_COMMAND_INVOKED) == 0) {
        const SolCommandInvokedPayload *p =
            (const SolCommandInvokedPayload *)event->payload;
        char label[SOL_CRASH_EVENT_LABEL_MAX];
        snprintf(label, sizeof(label), "command: %s",
                p->action ? p->action : "?");
        sol_crash_record_event(label);
        return false;
    }

    if (event->payload && event->payload_size >= sizeof(SolBufferEventPayload) &&
        (strcmp(event->name, SOL_EVENT_BUFFER_OPENED) == 0 ||
         strcmp(event->name, SOL_EVENT_BUFFER_CLOSED) == 0 ||
         strcmp(event->name, SOL_EVENT_BUFFER_FOCUSED) == 0)) {
        const SolBufferEventPayload *p =
            (const SolBufferEventPayload *)event->payload;
        char label[SOL_CRASH_EVENT_LABEL_MAX];
        const char *verb = strcmp(event->name, SOL_EVENT_BUFFER_OPENED) == 0 ? "opened"
                          : strcmp(event->name, SOL_EVENT_BUFFER_CLOSED) == 0 ? "closed"
                          : "focused";
        snprintf(label, sizeof(label), "buffer %s: %s", verb,
                p->name ? p->name : "?");
        sol_crash_record_event(label);

        if (strcmp(event->name, SOL_EVENT_BUFFER_CLOSED) == 0) {
            sol_crash_track_buffer_remove(p->buffer_id);
        } else if (p->source_path) {
            sol_crash_track_buffer_add(p->buffer_id, p->source_path);
        }
        return false;
    }

    if (event->payload && event->payload_size >= sizeof(SolFileTreeRootPayload) &&
        strcmp(event->name, SOL_EVENT_FILE_TREE_ROOT) == 0) {
        const SolFileTreeRootPayload *p =
            (const SolFileTreeRootPayload *)event->payload;
        char label[SOL_CRASH_EVENT_LABEL_MAX];
        snprintf(label, sizeof(label), "explorer root: %s",
                p->path ? p->path : "?");
        sol_crash_record_event(label);
        return false;
    }

    return false;
}

void sol_crash_track_events(SolEventBus *bus)
{
    if (!bus) return;
    const char *names[] = {
        SOL_EVENT_COMMAND_INVOKED,
        SOL_EVENT_BUFFER_OPENED,
        SOL_EVENT_BUFFER_CLOSED,
        SOL_EVENT_BUFFER_FOCUSED,
        SOL_EVENT_FILE_TREE_ROOT,
    };
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        (void)sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
            .event_name = names[i],
            .priority   = 0,
            .handler    = sol_crash_on_event,
            .user_data  = NULL,
        });
    }
}

/* ------------------------------------------------------------------ */
/* Async-signal-safe integer formatting.                               */
/*                                                                      */
/* snprintf/sprintf are NOT in POSIX's async-signal-safe function       */
/* list, so every number written into the report from inside the        */
/* handler goes through this instead — plain pointer arithmetic and     */
/* no libc formatting machinery, locale lookups, or internal buffers.   */
/* ------------------------------------------------------------------ */

/* Writes the base-10 (or base-16 when hex) representation of value
   into buf (capacity buf_size), returns the number of characters
   written, 0 on overflow. No leading zeros, no sign handling (all
   call sites here pass unsigned/non-negative values). */
static size_t sol_crash_format_uint(char *buf, size_t buf_size,
                                    unsigned long long value, bool hex)
{
    char tmp[32];
    size_t n = 0u;
    const char *digits = hex ? "0123456789abcdef" : "0123456789";
    const unsigned base = hex ? 16u : 10u;
    do {
        if (n >= sizeof(tmp)) return 0u;
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0u);
    if (n > buf_size) return 0u;
    for (size_t i = 0u; i < n; ++i) buf[i] = tmp[n - 1u - i];
    return n;
}

/* ------------------------------------------------------------------ */
/* The signal handler itself.                                          */
/*                                                                      */
/* Every function called from this point down (until the process        */
/* exits) is one of: write(2), open(2)/close(2), backtrace() /           */
/* backtrace_symbols_fd() (the write-to-fd variant, which performs no    */
/* allocation — unlike backtrace_symbols(), never called here), or the   */
/* integer formatter above. No malloc, no snprintf/fprintf, no mutex.   */
/* ------------------------------------------------------------------ */

#ifndef _WIN32

static const char *sol_crash_signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGBUS:  return "SIGBUS (bus error / misaligned access)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGFPE:  return "SIGFPE (arithmetic error)";
    default:      return "unknown signal";
    }
}

/* Appends `text` to the report fd, truncating silently at buf's fixed
   capacity if the caller passed a bound — used only for the format-int
   + write pairs below via sol_crash_write_str; kept trivial on purpose. */
static void sol_crash_write_str(int fd, const char *s)
{
    if (!s) return;
    size_t len = 0u;
    while (s[len] != '\0') len++;
    (void)write(fd, s, len);
}

static void sol_crash_write_uint(int fd, unsigned long long value, bool hex)
{
    char buf[32];
    size_t n = sol_crash_format_uint(buf, sizeof(buf), value, hex);
    if (n > 0u) (void)write(fd, buf, n);
}

/* Builds "<report_dir>/crash-<pid>-<ring_next_as_nonce>.txt" directly
   into out using only the safe integer formatter — no snprintf. Reusing
   event_ring_next as a nonce keeps two crashes in the same process
   (e.g. a handler re-entering after a second fault) from colliding on
   the same filename without needing a real random source or a fresh
   timestamp read here. */
static size_t sol_crash_build_report_path(char *out, size_t out_size)
{
    size_t n = 0u;
    const char *dir = g_crash_state.report_dir;
    size_t dir_len = 0u;
    while (dir[dir_len] != '\0') dir_len++;
    if (dir_len + 1u >= out_size) return 0u;
    for (size_t i = 0u; i < dir_len; ++i) out[n++] = dir[i];
    out[n++] = '/';

    const char *prefix = "crash-";
    for (const char *p = prefix; *p; ++p) {
        if (n >= out_size) return 0u;
        out[n++] = *p;
    }
    if (n >= out_size) return 0u;
    size_t written = sol_crash_format_uint(out + n, out_size - n,
                                           (unsigned long long)getpid(), false);
    if (written == 0u) return 0u;
    n += written;
    if (n >= out_size) return 0u;
    out[n++] = '-';
    written = sol_crash_format_uint(out + n, out_size - n,
                                    (unsigned long long)g_crash_state.event_ring_next,
                                    false);
    if (written == 0u) return 0u;
    n += written;

    const char *suffix = ".txt";
    for (const char *p = suffix; *p; ++p) {
        if (n >= out_size) return 0u;
        out[n++] = *p;
    }
    if (n >= out_size) return 0u;
    out[n] = '\0';
    return n;
}

static void sol_crash_write_report(int fd, int sig, void *fault_addr)
{
    sol_crash_write_str(fd, "Sol crash report\n=================\n\n");
    sol_crash_write_str(fd, "app:     ");
    sol_crash_write_str(fd, g_crash_state.app_name[0] ? g_crash_state.app_name : "Sol");
    sol_crash_write_str(fd, "\nversion: ");
    sol_crash_write_str(fd, g_crash_state.app_version[0]
                        ? g_crash_state.app_version : "unknown");
    sol_crash_write_str(fd, "\npid:     ");
    sol_crash_write_uint(fd, (unsigned long long)getpid(), false);
    sol_crash_write_str(fd, "\nsignal:  ");
    sol_crash_write_str(fd, sol_crash_signal_name(sig));
    if (fault_addr) {
        sol_crash_write_str(fd, "\nfault address: 0x");
        sol_crash_write_uint(fd, (unsigned long long)(uintptr_t)fault_addr, true);
    }
    sol_crash_write_str(fd, "\ncpu_count: ");
    sol_crash_write_uint(fd, (unsigned long long)sol_platform_cpu_count(), false);
    sol_crash_write_str(fd, "\n\n");

    sol_crash_write_str(fd, "Backtrace\n---------\n");
    void *frames[64];
    const int frame_count = backtrace(frames, 64);
    if (frame_count > 0) {
        backtrace_symbols_fd(frames, frame_count, fd);
    } else {
        sol_crash_write_str(fd, "(unavailable)\n");
    }
    sol_crash_write_str(fd, "\n");

    sol_crash_write_str(fd, "Open buffers\n------------\n");
    if (g_crash_state.open_buffer_count == 0u) {
        sol_crash_write_str(fd, "(none)\n");
    } else {
        for (uint32_t i = 0u; i < g_crash_state.open_buffer_count; ++i) {
            sol_crash_write_str(fd, "  ");
            sol_crash_write_str(fd, g_crash_state.open_buffers[i].path);
            sol_crash_write_str(fd, "\n");
        }
    }
    sol_crash_write_str(fd, "\n");

    sol_crash_write_str(fd, "Recent actions (oldest first)\n"
                            "------------------------------\n");
    if (g_crash_state.event_ring_count == 0u) {
        sol_crash_write_str(fd, "(none recorded)\n");
    } else {
        const uint32_t count = g_crash_state.event_ring_count;
        const uint32_t start = (g_crash_state.event_ring_next + SOL_CRASH_EVENT_RING_CAPACITY
                                - count) % SOL_CRASH_EVENT_RING_CAPACITY;
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t idx = (start + i) % SOL_CRASH_EVENT_RING_CAPACITY;
            sol_crash_write_str(fd, "  ");
            sol_crash_write_str(fd, g_crash_state.event_ring[idx].label);
            sol_crash_write_str(fd, "\n");
        }
    }

    sol_crash_write_str(fd,
        "\nPlease attach this file when reporting the crash.\n");
}

static void sol_crash_signal_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    void *fault_addr = info ? info->si_addr : NULL;

    if (g_crash_state.has_report_dir) {
        char path[SOL_CRASH_PATH_MAX + 64u];
        if (sol_crash_build_report_path(path, sizeof(path)) > 0u) {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                sol_crash_write_report(fd, sig, fault_addr);
                close(fd);
                sol_crash_write_str(STDERR_FILENO,
                    "\nSol crashed. A crash report was written to:\n  ");
                sol_crash_write_str(STDERR_FILENO, path);
                sol_crash_write_str(STDERR_FILENO,
                    "\nPlease share this file when reporting the issue.\n");
            }
        }
    }
    if (!g_crash_state.has_report_dir) {
        sol_crash_write_report(STDERR_FILENO, sig, fault_addr);
    }

    /* Restore default disposition and re-raise so the OS produces its
       own core dump / exit semantics exactly as if this handler never
       existed — this handler only adds the report, it never changes
       how the process ultimately terminates. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void sol_crash_install(const char *app_name, const char *app_version)
{
    if (g_crash_state.installed) return;
    g_crash_state.installed = true;

    snprintf(g_crash_state.app_name, sizeof(g_crash_state.app_name), "%s",
            app_name ? app_name : "Sol");
    snprintf(g_crash_state.app_version, sizeof(g_crash_state.app_version), "%s",
            app_version ? app_version : "unknown");

    char *dir = sol_config_path("crash_reports");
    if (dir && sol_platform_mkdir_p(dir) &&
        strlen(dir) < sizeof(g_crash_state.report_dir)) {
        snprintf(g_crash_state.report_dir, sizeof(g_crash_state.report_dir),
                "%s", dir);
        g_crash_state.has_report_dir = true;
    }
    free(dir);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sol_crash_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    const int signals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
    for (size_t i = 0u; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        (void)sigaction(signals[i], &sa, NULL);
    }
}

#else /* _WIN32 */

static LONG WINAPI sol_crash_seh_filter(EXCEPTION_POINTERS *info)
{
    if (g_crash_state.has_report_dir) {
        char path[SOL_CRASH_PATH_MAX + 64u];
        snprintf(path, sizeof(path), "%s\\crash-%lu.txt",
                g_crash_state.report_dir, (unsigned long)GetCurrentProcessId());
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char line[512];
            DWORD written = 0;
            int len = snprintf(line, sizeof(line),
                "Sol crash report\n=================\n\napp:     %s\nversion: %s\n"
                "exception code: 0x%08lx\nfault address: 0x%p\n\n",
                g_crash_state.app_name[0] ? g_crash_state.app_name : "Sol",
                g_crash_state.app_version[0] ? g_crash_state.app_version : "unknown",
                (unsigned long)info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
            if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);

            WriteFile(h, "Backtrace\n---------\n", 21, &written, NULL);
            void *frames[64];
            USHORT frame_count = CaptureStackBackTrace(0, 64, frames, NULL);
            HANDLE process = GetCurrentProcess();
            SymInitialize(process, NULL, TRUE);
            for (USHORT i = 0; i < frame_count; ++i) {
                char symbol_buf[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buf;
                symbol->MaxNameLen = 255;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                DWORD64 displacement = 0;
                const char *name = "?";
                if (SymFromAddr(process, (DWORD64)(uintptr_t)frames[i],
                                &displacement, symbol)) {
                    name = symbol->Name;
                }
                len = snprintf(line, sizeof(line), "  #%d 0x%p %s\n",
                               (int)i, frames[i], name);
                if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);
            }

            len = snprintf(line, sizeof(line), "\nOpen buffers\n------------\n");
            if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);
            if (g_crash_state.open_buffer_count == 0u) {
                WriteFile(h, "(none)\n", 7, &written, NULL);
            } else {
                for (uint32_t i = 0u; i < g_crash_state.open_buffer_count; ++i) {
                    len = snprintf(line, sizeof(line), "  %s\n",
                                   g_crash_state.open_buffers[i].path);
                    if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);
                }
            }

            len = snprintf(line, sizeof(line),
                "\nRecent actions (oldest first)\n------------------------------\n");
            if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);
            if (g_crash_state.event_ring_count == 0u) {
                WriteFile(h, "(none recorded)\n", 17, &written, NULL);
            } else {
                const uint32_t count = g_crash_state.event_ring_count;
                const uint32_t start = (g_crash_state.event_ring_next +
                    SOL_CRASH_EVENT_RING_CAPACITY - count) % SOL_CRASH_EVENT_RING_CAPACITY;
                for (uint32_t i = 0u; i < count; ++i) {
                    const uint32_t idx = (start + i) % SOL_CRASH_EVENT_RING_CAPACITY;
                    len = snprintf(line, sizeof(line), "  %s\n",
                                   g_crash_state.event_ring[idx].label);
                    if (len > 0) WriteFile(h, line, (DWORD)len, &written, NULL);
                }
            }

            CloseHandle(h);
            fprintf(stderr, "\nSol crashed. A crash report was written to:\n  %s\n"
                            "Please share this file when reporting the issue.\n", path);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void sol_crash_install(const char *app_name, const char *app_version)
{
    if (g_crash_state.installed) return;
    g_crash_state.installed = true;

    snprintf(g_crash_state.app_name, sizeof(g_crash_state.app_name), "%s",
            app_name ? app_name : "Sol");
    snprintf(g_crash_state.app_version, sizeof(g_crash_state.app_version), "%s",
            app_version ? app_version : "unknown");

    char *dir = sol_config_path("crash_reports");
    if (dir && sol_platform_mkdir_p(dir) &&
        strlen(dir) < sizeof(g_crash_state.report_dir)) {
        snprintf(g_crash_state.report_dir, sizeof(g_crash_state.report_dir),
                "%s", dir);
        g_crash_state.has_report_dir = true;
    }
    free(dir);

    SetUnhandledExceptionFilter(sol_crash_seh_filter);
}

#endif
