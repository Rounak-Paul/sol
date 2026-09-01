// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_file_watcher.h — Live filesystem change notifications for a
 * single watched directory tree.
 *
 * Backed by native per-platform APIs (FSEvents on macOS, inotify on
 * Linux, ReadDirectoryChangesW on Windows), each running on its own
 * background thread. Sol's UI (Causality) is single-threaded, so this
 * module never touches application state directly — it only appends
 * normalized {kind, path} events to an internal mutex-guarded queue.
 * The application drains that queue from the main thread once per
 * frame via sol_file_watcher_poll and reacts there (explorer refresh,
 * buffer reload) where it is safe to mutate UI/buffer state.
 *
 * Only one root is watched at a time (Sol has a single explorer root).
 * Calling sol_file_watcher_set_root again replaces the previous watch.
 */

#ifndef SOL_FILE_WATCHER_H
#define SOL_FILE_WATCHER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolFileWatcher SolFileWatcher;

/* Kind of change a single watch event represents. */
typedef enum SolFileWatchEventKind {
    SOL_FILE_WATCH_CHANGED = 0,  /* file content or metadata modified   */
    SOL_FILE_WATCH_CREATED,      /* file or directory created           */
    SOL_FILE_WATCH_REMOVED,      /* file or directory removed           */
    SOL_FILE_WATCH_RENAMED,      /* file or directory renamed/moved     */
} SolFileWatchEventKind;

/* Maximum path length carried in a single watch event, including NUL. */
#define SOL_FILE_WATCH_PATH_MAX 4096u

/* One coalesced filesystem change, ready for main-thread consumption. */
typedef struct SolFileWatchEvent {
    SolFileWatchEventKind kind;
    char                  path[SOL_FILE_WATCH_PATH_MAX];
} SolFileWatchEvent;

/*
 * Callback invoked from the watcher's background thread the moment a new
 * event is queued. Intended solely to wake an event-driven main loop
 * that would otherwise sleep until the next OS input event (e.g. via
 * ca_instance_wake) — it must be safe to call from any thread and must
 * not block or touch UI/buffer state itself (that state is only safe to
 * touch from the thread that later calls sol_file_watcher_poll).
 */
typedef void (*SolFileWatcherWakeFn)(void *user_data);

/*
 * Create an inactive file watcher.
 *
 * No background thread is started until sol_file_watcher_set_root is
 * called. Returns NULL on allocation failure.
 */
SolFileWatcher *sol_file_watcher_create(void);

/*
 * Register a callback fired from the background watch thread whenever a
 * new event is queued, so the caller can wake an idle-blocked event
 * loop. Pass NULL to clear. May be called before or after
 * sol_file_watcher_set_root.
 */
void sol_file_watcher_set_wake_callback(SolFileWatcher *watcher,
                                        SolFileWatcherWakeFn fn,
                                        void *user_data);

/*
 * Stop watching, join the background thread, and free all resources.
 *
 * Safe to call with watcher == NULL. Must be called before any state
 * the watcher's callback might otherwise reference (event bus, buffer
 * system) is torn down.
 */
void sol_file_watcher_destroy(SolFileWatcher *watcher);

/*
 * Start watching a directory tree, replacing any previously-watched root.
 *
 * Spawns (or restarts) the platform-native background watch. Pass NULL
 * or an empty string to stop watching without destroying the watcher.
 *
 * watcher  The file watcher.
 * path     Absolute path of the directory to watch recursively, or NULL
 *          to stop watching.
 * Returns  true on success; false if the platform watch could not be
 *          established (path missing, permission denied, OS resource
 *          limit). The explorer/editor remain fully usable either way —
 *          a failed watch just means no live updates.
 */
bool sol_file_watcher_set_root(SolFileWatcher *watcher, const char *path);

/*
 * Drain pending change events accumulated since the last call.
 *
 * Must be called from the main thread (the only thread permitted to
 * act on the results) once per frame. Bursts of native events for the
 * same path are coalesced, so a single save from an external editor
 * typically yields one event, not several.
 *
 * watcher     The file watcher.
 * out_events  Destination array.
 * max_events  Capacity of out_events.
 * Returns     Number of events written (0 when nothing changed or the
 *             watcher has no active root).
 */
size_t sol_file_watcher_poll(SolFileWatcher *watcher,
                             SolFileWatchEvent *out_events,
                             size_t max_events);

#ifdef __cplusplus
}
#endif

#endif /* SOL_FILE_WATCHER_H */
