// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_file_watcher.c — Native filesystem watcher.
 *
 * Shared, platform-agnostic piece: a small fixed-capacity mutex-guarded
 * queue that coalesces bursts of {kind, path} events. Every backend
 * (FSEvents / inotify / ReadDirectoryChangesW) only ever calls
 * sol_fw_queue_push from its own background thread; sol_file_watcher_poll
 * drains it from the main thread. This is the one new concurrency
 * primitive this codebase needed — deliberately minimal (no condvar,
 * since it is polled once per frame rather than blocked on).
 *
 * Platform backend, selected at compile time:
 *   __APPLE__  FSEventStreamCreate on a dedicated CFRunLoop thread.
 *   __linux__  inotify, one watch descriptor per directory (inotify has
 *              no native recursive watch), read loop on a pthread.
 *   _WIN32     ReadDirectoryChangesW with bWatchSubtree=TRUE (natively
 *              recursive) on an overlapped-I/O pthread (via sol_threading.h).
 */

#include "sol_file_watcher.h"

#include "sol_platform.h"
#include "sol_threading.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared coalescing queue                                             */
/* ------------------------------------------------------------------ */

#define SOL_FW_QUEUE_CAPACITY 256u

typedef struct SolFwQueue {
    pthread_mutex_t   mutex;
    SolFileWatchEvent items[SOL_FW_QUEUE_CAPACITY];
    size_t            count;
    /* Set when an event was dropped due to a full queue — surfaced to
       the caller as a single synthetic CHANGED event on the watched
       root itself, so a lost burst still results in *some* refresh
       rather than silently going stale forever. */
    bool              overflowed;
} SolFwQueue;

static void sol_fw_queue_init(SolFwQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
}

static void sol_fw_queue_destroy(SolFwQueue *q)
{
    pthread_mutex_destroy(&q->mutex);
}

/* Push one event, coalescing with the most recent queued event for the
   same path (a burst of writes to one file becomes a single CHANGED).
   Returns true when the queue transitioned from empty to non-empty
   (coalescing into an existing entry does not count), so the caller
   knows whether a wake is actually needed — the main loop only needs
   nudging when it might otherwise stay asleep with fresh work waiting. */
static bool sol_fw_queue_push(SolFwQueue *q, SolFileWatchEventKind kind,
                              const char *path)
{
    if (!q || !path || !path[0]) return false;

    pthread_mutex_lock(&q->mutex);
    for (size_t i = 0u; i < q->count; ++i) {
        if (strcmp(q->items[i].path, path) == 0) {
            q->items[i].kind = kind;   /* last write wins */
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
    }
    const bool was_empty = (q->count == 0u);
    if (q->count >= SOL_FW_QUEUE_CAPACITY) {
        q->overflowed = true;
        pthread_mutex_unlock(&q->mutex);
        return was_empty;
    }
    SolFileWatchEvent *slot = &q->items[q->count++];
    slot->kind = kind;
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    pthread_mutex_unlock(&q->mutex);
    return was_empty;
}

static size_t sol_fw_queue_drain(SolFwQueue *q, const char *root_for_overflow,
                                 SolFileWatchEvent *out, size_t max_out)
{
    if (!q || !out || max_out == 0u) return 0u;

    pthread_mutex_lock(&q->mutex);
    size_t n = q->count < max_out ? q->count : max_out;
    memcpy(out, q->items, n * sizeof(SolFileWatchEvent));
    const bool overflowed = q->overflowed;
    q->count = 0u;
    q->overflowed = false;
    pthread_mutex_unlock(&q->mutex);

    if (overflowed && n < max_out && root_for_overflow && root_for_overflow[0]) {
        out[n].kind = SOL_FILE_WATCH_CHANGED;
        snprintf(out[n].path, sizeof(out[n].path), "%s", root_for_overflow);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Watcher handle                                                      */
/* ------------------------------------------------------------------ */

struct SolFileWatcher {
    SolFwQueue queue;
    char       root[SOL_FILE_WATCH_PATH_MAX];
    bool       active;

    /* Set once from the main thread before/while the watch is active;
       read from the background watch thread on every push. A plain
       (non-atomic) pointer/function-pointer pair is safe here in
       practice — sol_file_watcher_set_wake_callback is documented to be
       called before or right after set_root, not concurrently raced
       against an in-flight callback — but callers should still avoid
       calling it while a watch is actively delivering events. */
    SolFileWatcherWakeFn wake_fn;
    void                 *wake_user_data;

#if defined(__APPLE__)
    void *fsevent_stream;    /* FSEventStreamRef, opaque here to keep this
                                 header CoreFoundation-free */
    void *dispatch_queue;    /* dispatch_queue_t the stream delivers on */
#elif defined(__linux__)
    int          inotify_fd;
    /* Self-pipe used to reliably wake the reader thread out of poll().
       Closing inotify_fd from another thread does NOT interrupt a
       concurrent blocking read()/poll() on it on Linux — the reader
       already holds its own kernel reference from syscall entry, so it
       just keeps waiting for a real event that never arrives. Writing a
       byte to wake_fd[1] is the portable way to force an immediate,
       guaranteed wakeup. wake_fd[0]/[1] are -1 when not initialized. */
    int          wake_fd[2];
    pthread_t    reader_thread;
    atomic_bool  stop_reader;
    atomic_bool  reader_running;
    /* wd -> absolute path, so IN_CREATE on a subdirectory can be turned
       into a new watch and so events can be resolved back to a path
       (inotify events only carry the watched dir's wd + a basename). */
    struct { int wd; char *path; } *watches;
    size_t watch_count;
    size_t watch_capacity;
#elif defined(_WIN32)
    pthread_t    reader_thread;
    atomic_bool  stop_reader;
    atomic_bool  reader_running;
    void        *dir_handle;   /* HANDLE, void* to avoid windows.h in the header */
#endif
};

/* Push one event and, if that woke the queue from empty, fire the
   registered wake callback (if any) so an idle-blocked main loop can be
   nudged. Called only from the background watch thread. */
static void sol_fw_push_and_wake(SolFileWatcher *watcher,
                                 SolFileWatchEventKind kind, const char *path)
{
    const bool became_non_empty = sol_fw_queue_push(&watcher->queue, kind, path);
    if (became_non_empty && watcher->wake_fn) {
        watcher->wake_fn(watcher->wake_user_data);
    }
}

/* ------------------------------------------------------------------ */
/* macOS backend — FSEvents                                            */
/* ------------------------------------------------------------------ */
#if defined(__APPLE__)

#include <CoreServices/CoreServices.h>

static void sol_fw_fsevents_callback(ConstFSEventStreamRef stream,
                                     void *client_data,
                                     size_t num_events,
                                     void *event_paths,
                                     const FSEventStreamEventFlags event_flags[],
                                     const FSEventStreamEventId event_ids[])
{
    (void)stream; (void)event_ids;
    SolFileWatcher *watcher = (SolFileWatcher *)client_data;
    char **paths = (char **)event_paths;
    for (size_t i = 0u; i < num_events; ++i) {
        SolFileWatchEventKind kind = SOL_FILE_WATCH_CHANGED;
        const FSEventStreamEventFlags f = event_flags[i];
        if (f & (kFSEventStreamEventFlagItemRemoved)) kind = SOL_FILE_WATCH_REMOVED;
        else if (f & kFSEventStreamEventFlagItemCreated) kind = SOL_FILE_WATCH_CREATED;
        else if (f & kFSEventStreamEventFlagItemRenamed) kind = SOL_FILE_WATCH_RENAMED;
        sol_fw_push_and_wake(watcher, kind, paths[i]);
    }
}

/* Uses FSEventStreamSetDispatchQueue (the current, non-deprecated API)
   rather than scheduling on a CFRunLoop — a dedicated serial GCD queue
   plays the same role as the CFRunLoop-thread approach without owning
   thread lifecycle by hand. dispatch_release is a no-op under ARC-less
   plain C with libdispatch's default retain/release semantics on this
   platform, so the queue is explicitly released in stop(). */
static bool sol_fw_platform_start(SolFileWatcher *watcher)
{
    CFStringRef path_ref = CFStringCreateWithCString(
        kCFAllocatorDefault, watcher->root, kCFStringEncodingUTF8);
    if (!path_ref) return false;
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault,
                                     (const void **)&path_ref, 1, &kCFTypeArrayCallBacks);
    CFRelease(path_ref);
    if (!paths) return false;

    FSEventStreamContext ctx = {0};
    ctx.info = watcher;

    FSEventStreamRef stream = FSEventStreamCreate(
        kCFAllocatorDefault, sol_fw_fsevents_callback, &ctx, paths,
        kFSEventStreamEventIdSinceNow, 0.3 /* latency seconds */,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (!stream) return false;

    dispatch_queue_t queue = dispatch_queue_create("sol.file_watcher", DISPATCH_QUEUE_SERIAL);
    if (!queue) {
        FSEventStreamRelease(stream);
        return false;
    }
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream)) {
        FSEventStreamSetDispatchQueue(stream, NULL);
        FSEventStreamRelease(stream);
        dispatch_release(queue);
        return false;
    }

    watcher->fsevent_stream = stream;
    watcher->dispatch_queue = (void *)queue;
    return true;
}

static void sol_fw_platform_stop(SolFileWatcher *watcher)
{
    if (watcher->fsevent_stream) {
        FSEventStreamRef stream = (FSEventStreamRef)watcher->fsevent_stream;
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        watcher->fsevent_stream = NULL;
    }
    if (watcher->dispatch_queue) {
        dispatch_release((dispatch_queue_t)watcher->dispatch_queue);
        watcher->dispatch_queue = NULL;
    }
}

#endif /* __APPLE__ */

/* ------------------------------------------------------------------ */
/* Linux backend — inotify                                             */
/* ------------------------------------------------------------------ */
#if defined(__linux__)

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOL_FW_INOTIFY_MASK \
    (IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB)

static bool sol_fw_add_watch_entry(SolFileWatcher *watcher, int wd, const char *path)
{
    if (watcher->watch_count >= watcher->watch_capacity) {
        size_t new_cap = watcher->watch_capacity ? watcher->watch_capacity * 2u : 64u;
        void *grown = realloc(watcher->watches, new_cap * sizeof(*watcher->watches));
        if (!grown) return false;
        watcher->watches = grown;
        watcher->watch_capacity = new_cap;
    }
    char *owned = strdup(path);
    if (!owned) return false;
    watcher->watches[watcher->watch_count].wd = wd;
    watcher->watches[watcher->watch_count].path = owned;
    watcher->watch_count++;
    return true;
}

static const char *sol_fw_path_for_wd(SolFileWatcher *watcher, int wd)
{
    for (size_t i = 0u; i < watcher->watch_count; ++i) {
        if (watcher->watches[i].wd == wd) return watcher->watches[i].path;
    }
    return NULL;
}

/* Recursively add an inotify watch for dir_path and every subdirectory
   beneath it. inotify has no native recursive mode, so each directory
   needs its own watch descriptor.

   Uses lstat (not the entry iterator's stat-following is_directory) to
   skip symlinked directories: neither FSEvents nor ReadDirectoryChangesW
   follows symlinks into a tree, and doing so here has no cycle detection
   — a self- or mutually-referential symlink (common in node_modules,
   build output, etc.) would otherwise recurse until inotify's per-user
   watch limit is exhausted, or effectively forever on a deep enough
   cycle.

   Checks stop_reader between entries so a watcher torn down mid-descent
   (e.g. rapidly switching the explorer root) unwinds promptly instead of
   finishing an entire stale tree walk on the way out. */
static void sol_fw_watch_recursive(SolFileWatcher *watcher, const char *dir_path)
{
    if (atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed)) return;

    struct stat st;
    if (lstat(dir_path, &st) != 0 || S_ISLNK(st.st_mode)) return;

    int wd = inotify_add_watch(watcher->inotify_fd, dir_path, SOL_FW_INOTIFY_MASK);
    if (wd < 0) return;
    if (!sol_fw_add_watch_entry(watcher, wd, dir_path)) {
        inotify_rm_watch(watcher->inotify_fd, wd);
        return;
    }

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, dir_path)) return;
    SolDirectoryEntry entry;
    while (!atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed) &&
           sol_platform_dir_next(&iter, &entry)) {
        if (!entry.is_directory) continue;
        char *child = sol_platform_path_join(dir_path, entry.name);
        if (!child) continue;
        sol_fw_watch_recursive(watcher, child);
        free(child);
    }
    sol_platform_dir_close(&iter);
}

/* Recurse into every subdirectory of dir_path, adding a watch for each,
   without re-watching dir_path itself (already watched by the caller).
   Used for the deferred initial descent from the watch root — see
   sol_fw_linux_reader_thread. */
static void sol_fw_watch_children(SolFileWatcher *watcher, const char *dir_path)
{
    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, dir_path)) return;
    SolDirectoryEntry entry;
    while (!atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed) &&
           sol_platform_dir_next(&iter, &entry)) {
        if (!entry.is_directory) continue;
        char *child = sol_platform_path_join(dir_path, entry.name);
        if (!child) continue;
        sol_fw_watch_recursive(watcher, child);
        free(child);
    }
    sol_platform_dir_close(&iter);
}

static void *sol_fw_linux_reader_thread(void *arg)
{
    SolFileWatcher *watcher = (SolFileWatcher *)arg;
    atomic_store_explicit(&watcher->reader_running, true, memory_order_release);

    /* The recursive descent that establishes per-subdirectory watches
       happens here, on the background thread, rather than in
       sol_fw_platform_start on the caller's thread — a large tree (deep
       node_modules, monorepo checkouts, etc.) can mean thousands of
       inotify_add_watch + readdir syscalls, which would otherwise
       freeze the caller (typically the UI thread, via
       sol_file_watcher_set_root) for the entire walk. The root watch
       itself is already established synchronously in
       sol_fw_platform_start before this thread is spawned, so events
       under the root are never missed — only the deeper subdirectory
       watches lag slightly behind set_root returning. */
    sol_fw_watch_children(watcher, watcher->root);

    char buf[64 * (sizeof(struct inotify_event) + NAME_MAX + 1)];
    while (!atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed)) {
        struct pollfd fds[2];
        fds[0].fd = watcher->inotify_fd; fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = watcher->wake_fd[0]; fds[1].events = POLLIN; fds[1].revents = 0;

        int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Woken by stop() writing to the self-pipe — exit without touching
           inotify_fd, which the caller may be about to close. */
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t n = read(watcher->inotify_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;   /* fd closed by destroy(), or a real error — stop either way */
        }

        ssize_t off = 0;
        while (off < n) {
            const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
            const char *dir = sol_fw_path_for_wd(watcher, ev->wd);
            if (dir && ev->len > 0u) {
                char full[SOL_FILE_WATCH_PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", dir, ev->name);

                SolFileWatchEventKind kind = SOL_FILE_WATCH_CHANGED;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO)) kind = SOL_FILE_WATCH_CREATED;
                else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) kind = SOL_FILE_WATCH_REMOVED;

                if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                    sol_fw_watch_recursive(watcher, full);
                }
                sol_fw_push_and_wake(watcher, kind, full);
            }
            off += (ssize_t)(sizeof(struct inotify_event) + ev->len);
        }
    }

    atomic_store_explicit(&watcher->reader_running, false, memory_order_release);
    return NULL;
}

static bool sol_fw_platform_start(SolFileWatcher *watcher)
{
    watcher->inotify_fd = inotify_init1(0);
    if (watcher->inotify_fd < 0) return false;

    watcher->wake_fd[0] = watcher->wake_fd[1] = -1;
    if (pipe(watcher->wake_fd) != 0) {
        close(watcher->inotify_fd);
        watcher->inotify_fd = -1;
        return false;
    }

    /* Only the root watch is established synchronously here, so a bad
       root (permission denied, watch-limit exhaustion) still fails
       set_root immediately as documented. The recursive descent into
       subdirectories runs on the reader thread instead (see its top) —
       for a large tree it can mean thousands of syscalls, which must
       not block the caller (typically the UI thread). */
    int root_wd = inotify_add_watch(watcher->inotify_fd, watcher->root, SOL_FW_INOTIFY_MASK);
    if (root_wd < 0 || !sol_fw_add_watch_entry(watcher, root_wd, watcher->root)) {
        if (root_wd >= 0) inotify_rm_watch(watcher->inotify_fd, root_wd);
        close(watcher->inotify_fd);
        watcher->inotify_fd = -1;
        close(watcher->wake_fd[0]);
        close(watcher->wake_fd[1]);
        watcher->wake_fd[0] = watcher->wake_fd[1] = -1;
        return false;
    }

    atomic_store_explicit(&watcher->stop_reader, false, memory_order_relaxed);
    if (pthread_create(&watcher->reader_thread, NULL,
                       sol_fw_linux_reader_thread, watcher) != 0) {
        close(watcher->inotify_fd);
        watcher->inotify_fd = -1;
        close(watcher->wake_fd[0]);
        close(watcher->wake_fd[1]);
        watcher->wake_fd[0] = watcher->wake_fd[1] = -1;
        return false;
    }
    return true;
}

static void sol_fw_platform_stop(SolFileWatcher *watcher)
{
    atomic_store_explicit(&watcher->stop_reader, true, memory_order_relaxed);
    if (watcher->wake_fd[1] >= 0) {
        /* Guaranteed wakeup for the reader's poll(), unlike closing
           inotify_fd (see the wake_fd comment on the struct). */
        char b = 0;
        ssize_t written = write(watcher->wake_fd[1], &b, 1u);
        (void)written;
    }
    if (atomic_load_explicit(&watcher->reader_running, memory_order_acquire) ||
        watcher->reader_thread) {
        pthread_join(watcher->reader_thread, NULL);
    }
    if (watcher->inotify_fd >= 0) {
        close(watcher->inotify_fd);
        watcher->inotify_fd = -1;
    }
    if (watcher->wake_fd[0] >= 0) { close(watcher->wake_fd[0]); watcher->wake_fd[0] = -1; }
    if (watcher->wake_fd[1] >= 0) { close(watcher->wake_fd[1]); watcher->wake_fd[1] = -1; }
    for (size_t i = 0u; i < watcher->watch_count; ++i) {
        free(watcher->watches[i].path);
    }
    free(watcher->watches);
    watcher->watches = NULL;
    watcher->watch_count = 0u;
    watcher->watch_capacity = 0u;
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* Windows backend — ReadDirectoryChangesW                             */
/* ------------------------------------------------------------------ */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static SolFileWatchEventKind sol_fw_kind_from_win32(DWORD action)
{
    switch (action) {
    case FILE_ACTION_ADDED:            return SOL_FILE_WATCH_CREATED;
    case FILE_ACTION_REMOVED:          return SOL_FILE_WATCH_REMOVED;
    case FILE_ACTION_RENAMED_OLD_NAME: return SOL_FILE_WATCH_REMOVED;
    case FILE_ACTION_RENAMED_NEW_NAME: return SOL_FILE_WATCH_RENAMED;
    default:                           return SOL_FILE_WATCH_CHANGED;
    }
}

static void *sol_fw_win32_reader_thread(void *arg)
{
    SolFileWatcher *watcher = (SolFileWatcher *)arg;
    atomic_store_explicit(&watcher->reader_running, true, memory_order_release);

    HANDLE dir = (HANDLE)watcher->dir_handle;
    BYTE buf[64 * 1024];

    while (!atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed)) {
        DWORD bytes = 0u;
        BOOL ok = ReadDirectoryChangesW(
            dir, buf, sizeof(buf), /* bWatchSubtree */ TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            &bytes, NULL, NULL);
        if (!ok || bytes == 0u) {
            /* Handle closed by destroy(), or a transient error — either
               way, re-check the stop flag rather than spinning tight. */
            if (atomic_load_explicit(&watcher->stop_reader, memory_order_relaxed)) break;
            continue;
        }

        DWORD offset = 0u;
        for (;;) {
            FILE_NOTIFY_INFORMATION *info =
                (FILE_NOTIFY_INFORMATION *)(buf + offset);

            char narrow_name[SOL_FILE_WATCH_PATH_MAX];
            int wn = WideCharToMultiByte(
                CP_UTF8, 0, info->FileName,
                (int)(info->FileNameLength / sizeof(WCHAR)),
                narrow_name, (int)sizeof(narrow_name) - 1, NULL, NULL);
            if (wn > 0) {
                narrow_name[wn] = '\0';
                char full[SOL_FILE_WATCH_PATH_MAX];
                snprintf(full, sizeof(full), "%s\\%s", watcher->root, narrow_name);
                sol_fw_push_and_wake(watcher,
                                    sol_fw_kind_from_win32(info->Action), full);
            }

            if (info->NextEntryOffset == 0u) break;
            offset += info->NextEntryOffset;
        }
    }

    atomic_store_explicit(&watcher->reader_running, false, memory_order_release);
    return NULL;
}

static bool sol_fw_platform_start(SolFileWatcher *watcher)
{
    HANDLE dir = CreateFileA(
        watcher->root, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dir == INVALID_HANDLE_VALUE) return false;
    watcher->dir_handle = (void *)dir;

    atomic_store_explicit(&watcher->stop_reader, false, memory_order_relaxed);
    if (pthread_create(&watcher->reader_thread, NULL,
                       sol_fw_win32_reader_thread, watcher) != 0) {
        CloseHandle(dir);
        watcher->dir_handle = NULL;
        return false;
    }
    return true;
}

static void sol_fw_platform_stop(SolFileWatcher *watcher)
{
    atomic_store_explicit(&watcher->stop_reader, true, memory_order_relaxed);
    if (watcher->dir_handle) {
        /* Closing the handle aborts the pending ReadDirectoryChangesW
           call, unblocking the reader thread. */
        CloseHandle((HANDLE)watcher->dir_handle);
        watcher->dir_handle = NULL;
    }
    if (atomic_load_explicit(&watcher->reader_running, memory_order_acquire) ||
        watcher->reader_thread) {
        pthread_join(watcher->reader_thread, NULL);
    }
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Fallback — no watcher support on this platform                      */
/* ------------------------------------------------------------------ */
#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)

static bool sol_fw_platform_start(SolFileWatcher *watcher)
{
    (void)watcher;
    return false;
}

static void sol_fw_platform_stop(SolFileWatcher *watcher)
{
    (void)watcher;
}

#endif

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

SolFileWatcher *sol_file_watcher_create(void)
{
    SolFileWatcher *watcher = (SolFileWatcher *)calloc(1u, sizeof(SolFileWatcher));
    if (!watcher) return NULL;
    sol_fw_queue_init(&watcher->queue);
#if defined(__linux__)
    watcher->inotify_fd = -1;
    watcher->wake_fd[0] = watcher->wake_fd[1] = -1;
#endif
    return watcher;
}

void sol_file_watcher_destroy(SolFileWatcher *watcher)
{
    if (!watcher) return;
    if (watcher->active) {
        sol_fw_platform_stop(watcher);
    }
    sol_fw_queue_destroy(&watcher->queue);
    free(watcher);
}

void sol_file_watcher_set_wake_callback(SolFileWatcher *watcher,
                                        SolFileWatcherWakeFn fn,
                                        void *user_data)
{
    if (!watcher) return;
    watcher->wake_fn = fn;
    watcher->wake_user_data = user_data;
}

bool sol_file_watcher_set_root(SolFileWatcher *watcher, const char *path)
{
    if (!watcher) return false;

    if (watcher->active) {
        sol_fw_platform_stop(watcher);
        watcher->active = false;
    }
    watcher->root[0] = '\0';

    if (!path || !path[0]) return true;   /* stop-only request succeeded */

    SolPathInfo info;
    if (!sol_platform_get_path_info(path, &info) || !info.is_directory) {
        return false;
    }

    snprintf(watcher->root, sizeof(watcher->root), "%s", path);
    watcher->active = sol_fw_platform_start(watcher);
    if (!watcher->active) {
        watcher->root[0] = '\0';
    }
    return watcher->active;
}

size_t sol_file_watcher_poll(SolFileWatcher *watcher,
                             SolFileWatchEvent *out_events,
                             size_t max_events)
{
    if (!watcher || !watcher->active) return 0u;
    return sol_fw_queue_drain(&watcher->queue, watcher->root, out_events, max_events);
}
