#ifndef SOL_PLATFORM_H
#define SOL_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* File-system metadata for a path, returned by sol_platform_get_path_info. */
typedef struct SolPathInfo {
    bool exists;
    bool is_directory;
    bool is_regular_file;
    uint64_t size_bytes;
} SolPathInfo;

/* A single entry yielded by the directory iterator. */
typedef struct SolDirectoryEntry {
    const char *name;
    bool is_directory;
} SolDirectoryEntry;

/* Opaque iterator for walking the entries of a directory. */
typedef struct SolDirectoryIter {
#if defined(_WIN32)
    void *handle;
    struct {
        unsigned int dwFileAttributes;
        char cFileName[260];
    } find_data;
    bool has_pending;
#else
    void *dir;
#endif
    char *base_path;
} SolDirectoryIter;

/* A read-only memory-mapped file opened by sol_platform_map_file_readonly. */
typedef struct SolMappedFile {
    const uint8_t *data;
    size_t size_bytes;
#if defined(_WIN32)
    void *file_handle;
    void *mapping_handle;
#else
    void *mapping_base;
#endif
} SolMappedFile;

/* Returns the platform path separator character ('/' on POSIX, '\\' on Win32). */
char sol_platform_path_separator(void);

/* Returns true if c is a valid path separator on the current platform. */
bool sol_platform_is_path_separator(char c);

/*
 * Join two path components into a newly-allocated string.
 *
 * a        Left path component.
 * b        Right path component.
 * Returns  Heap-allocated joined path; caller must free().
 */
char *sol_platform_path_join(const char *a, const char *b);

/*
 * Return a pointer to the basename portion of a path (no allocation).
 *
 * path    Full path string.
 * Returns Pointer into path at the start of the final component.
 */
const char *sol_platform_basename(const char *path);

/*
 * Write the current working directory into buffer.
 *
 * buffer       Destination buffer for a null-terminated path.
 * buffer_size  Total bytes available in buffer.
 * Returns      true on success.
 */
bool sol_platform_get_cwd(char *buffer, size_t buffer_size);

/*
 * Write the absolute path of the running executable into buffer.
 *
 * buffer       Destination buffer for a null-terminated path.
 * buffer_size  Total bytes available in buffer.
 * Returns      true on success.
 */
bool sol_platform_get_executable_path(char *buffer, size_t buffer_size);

/*
 * Return the platform-standard config home directory as a heap string.
 *
 * Returns  Heap-allocated path the caller must free(); NULL on failure.
 */
char *sol_platform_config_home_dir(void);

/*
 * Retrieve metadata for the given path.
 *
 * path      Path to query.
 * out_info  Receives exists, kind, and size information.
 * Returns   true on success (including when the path does not exist).
 */
bool sol_platform_get_path_info(const char *path, SolPathInfo *out_info);

/*
 * Create a directory and all missing parent directories.
 *
 * path    Directory path to create.
 * Returns true if the directory exists (or was created) on return.
 */
bool sol_platform_mkdir_p(const char *path);

/*
 * Create an empty file at the given path.
 *
 * path            Path of the file to create.
 * fail_if_exists  If true, returns false when the file already exists.
 * Returns         true on success.
 */
bool sol_platform_create_empty_file(const char *path, bool fail_if_exists);

/*
 * Recursively remove a file or directory tree.
 *
 * path    Root of the tree to remove.
 * Returns true if the path no longer exists after the call.
 */
bool sol_platform_remove_path_recursive(const char *path);

/*
 * Recursively copy a file or directory tree.
 *
 * source_path  Path to copy from.
 * dest_path    Path to copy to (created if absent).
 * Returns      true on success.
 */
bool sol_platform_copy_path_recursive(const char *source_path, const char *dest_path);

/*
 * Move (rename) a file or directory.
 *
 * source_path  Existing path.
 * dest_path    Destination path.
 * Returns      true on success.
 */
bool sol_platform_move_path(const char *source_path, const char *dest_path);

/*
 * Atomically replace dest_path with temp_path, in a single filesystem
 * operation. Intended for crash-safe file saves: write new content to a
 * temp file in the same directory as the destination, then call this to
 * publish it. On POSIX this is plain rename() (atomic when both paths
 * are on the same filesystem). On Win32, rename() fails when the
 * destination already exists, so this uses MoveFileExA with
 * MOVEFILE_REPLACE_EXISTING instead. Unlike sol_platform_move_path,
 * this never falls back to copy+delete — if the atomic replace fails,
 * the destination is guaranteed unchanged and temp_path is left in
 * place for the caller to clean up.
 *
 * temp_path   Existing file whose content should become dest_path.
 * dest_path   Path to atomically replace (may or may not already exist).
 * Returns     true on success.
 */
bool sol_platform_replace_file(const char *temp_path, const char *dest_path);

/* Returns the number of logical CPU cores available to the process. */
uint32_t sol_platform_cpu_count(void);

/* Returns the current value of a monotonic high-resolution clock in nanoseconds. */
uint64_t sol_platform_now_monotonic_ns(void);

/*
 * Load a dynamic library by path.
 *
 * path    Path to the shared library file.
 * Returns An opaque handle, or NULL on failure.
 */
void *sol_platform_library_open(const char *path);

/*
 * Look up an exported symbol in a loaded library.
 *
 * library  Handle returned by sol_platform_library_open.
 * symbol   Name of the exported symbol.
 * Returns  Pointer to the symbol, or NULL if not found.
 */
void *sol_platform_library_symbol(void *library, const char *symbol);

/*
 * Unload a previously opened library.
 *
 * library  Handle returned by sol_platform_library_open.
 * Returns  true on success.
 */
bool sol_platform_library_close(void *library);

/* Returns a human-readable description of the last library error (thread-local). */
const char *sol_platform_library_last_error(void);

/* Returns the file extension used for dynamic libraries on this platform (".dylib", ".so", ".dll"). */
const char *sol_platform_dynamic_library_extension(void);

/*
 * Open a directory for iteration.
 *
 * iter  Iterator to initialise.
 * path  Directory path to open.
 * Returns  true on success.
 */
bool sol_platform_dir_open(SolDirectoryIter *iter, const char *path);

/*
 * Advance to the next directory entry.
 *
 * iter   An iterator opened by sol_platform_dir_open.
 * entry  Receives the name and kind of the next entry.
 * Returns  true while entries remain; false when exhausted.
 */
bool sol_platform_dir_next(SolDirectoryIter *iter, SolDirectoryEntry *entry);

/* Close a directory iterator and release its resources. */
void sol_platform_dir_close(SolDirectoryIter *iter);

/*
 * Open a file as a read-only memory mapping.
 *
 * path       Path of the file to map.
 * out_file   Receives the mapped data pointer and size.
 * out_error  If non-NULL, receives a static error string on failure.
 * Returns    true on success.
 */
bool sol_platform_map_file_readonly(const char *path, SolMappedFile *out_file, const char **out_error);

/*
 * Release a memory-mapped file opened by sol_platform_map_file_readonly.
 *
 * mapped_file  The mapping to release.
 */
void sol_platform_unmap_file(SolMappedFile *mapped_file);

#endif
