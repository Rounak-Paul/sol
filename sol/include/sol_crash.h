// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_crash.h — fatal-signal capture and crash report generation.
 *
 * Installs handlers for the process's fatal signals (SIGSEGV, SIGABRT,
 * SIGBUS, SIGILL, SIGFPE on POSIX; the unhandled-exception filter on
 * Windows). On a crash, writes a human-readable report to
 * ~/.sol/crash_reports/ containing the signal, a symbolized backtrace,
 * a trail of recent user actions, and the set of buffers open at the
 * time — everything a developer needs to reproduce the bug, without
 * needing the user to also attach a debugger or remember what they were
 * doing.
 *
 * Signal-handler safety: everything read inside the handler is a plain
 * static/global value written by the normal (non-crashing) code path
 * ahead of time via sol_crash_record_event / sol_crash_track_buffer_*.
 * The handler itself performs no heap allocation and calls only
 * functions POSIX lists as async-signal-safe (write, open, close,
 * _exit, backtrace_symbols_fd) — see sol_crash.c for the full
 * accounting of which calls only run on the non-crashing path.
 */
#pragma once

#include "sol_buffer.h"

#include <stdbool.h>

typedef struct SolEventBus SolEventBus;

/*
 * Install the process-wide fatal-signal / unhandled-exception handlers.
 *
 * Safe to call once, early in main() — before any other subsystem that
 * might crash is created, so a fault during their own init is still
 * caught. Idempotent guards are not provided; call exactly once.
 *
 * app_name          Short name to embed in the report header (e.g. "Sol").
 * app_version       Short build identifier to embed (e.g. a git short
 *                    hash); may be NULL for "unknown".
 */
void sol_crash_install(const char *app_name, const char *app_version);

/*
 * Subscribe the crash tracker to the event types it uses to build the
 * "recent actions" trail and the open-buffer snapshot
 * (SOL_EVENT_COMMAND_INVOKED, SOL_EVENT_BUFFER_OPENED/CLOSED/FOCUSED,
 * SOL_EVENT_FILE_TREE_ROOT). Call once after the event bus exists.
 *
 * bus  The application's event bus.
 */
void sol_crash_track_events(SolEventBus *bus);

/*
 * Record one free-form breadcrumb into the crash tracker's fixed-size
 * ring buffer, shown in the report as the "recent actions" trail.
 *
 * Intended for the small set of high-signal events wired up by
 * sol_crash_track_events; exposed separately so a caller with something
 * else worth recording (a plugin action, a file-watcher event) is not
 * forced to route it through the event bus first.
 *
 * label  Short human-readable description, e.g. "command: buffer.save".
 *        Copied into a fixed-size slot — long labels are truncated.
 */
void sol_crash_record_event(const char *label);
