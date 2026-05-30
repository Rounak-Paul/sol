#ifndef SOL_PLATFORM_H
#define SOL_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SolPathInfo {
    bool exists;
    bool is_directory;
    bool is_regular_file;
    uint64_t size_bytes;
} SolPathInfo;

typedef struct SolDirectoryEntry {
    const char *name;
    bool is_directory;
} SolDirectoryEntry;

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

char sol_platform_path_separator(void);
bool sol_platform_is_path_separator(char c);
char *sol_platform_path_join(const char *a, const char *b);
const char *sol_platform_basename(const char *path);

bool sol_platform_get_cwd(char *buffer, size_t buffer_size);
char *sol_platform_config_home_dir(void);
bool sol_platform_get_path_info(const char *path, SolPathInfo *out_info);
bool sol_platform_mkdir_p(const char *path);

uint32_t sol_platform_cpu_count(void);
uint64_t sol_platform_now_monotonic_ns(void);

void *sol_platform_library_open(const char *path);
void *sol_platform_library_symbol(void *library, const char *symbol);
bool sol_platform_library_close(void *library);
const char *sol_platform_library_last_error(void);
const char *sol_platform_dynamic_library_extension(void);

bool sol_platform_dir_open(SolDirectoryIter *iter, const char *path);
bool sol_platform_dir_next(SolDirectoryIter *iter, SolDirectoryEntry *entry);
void sol_platform_dir_close(SolDirectoryIter *iter);

bool sol_platform_map_file_readonly(const char *path, SolMappedFile *out_file, const char **out_error);
void sol_platform_unmap_file(SolMappedFile *mapped_file);

#endif
