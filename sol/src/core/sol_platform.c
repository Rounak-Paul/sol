#include "sol_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <io.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
static char g_sol_dl_error[512];

static void sol_platform_copy_find_data(SolDirectoryIter *iter, const WIN32_FIND_DATAA *data)
{
    iter->find_data.dwFileAttributes = data->dwFileAttributes;
    strncpy(iter->find_data.cFileName, data->cFileName, sizeof(iter->find_data.cFileName) - 1u);
    iter->find_data.cFileName[sizeof(iter->find_data.cFileName) - 1u] = '\0';
}
#endif

char sol_platform_path_separator(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

bool sol_platform_is_path_separator(char c)
{
    return c == '/' || c == '\\';
}

char *sol_platform_path_join(const char *a, const char *b)
{
    if (!a || !b) {
        return NULL;
    }

    const size_t la = strlen(a);
    const size_t lb = strlen(b);
    const bool need_sep = la > 0u && !sol_platform_is_path_separator(a[la - 1u]);

    char *out = (char *)malloc(la + (need_sep ? 1u : 0u) + lb + 1u);
    if (!out) {
        return NULL;
    }

    memcpy(out, a, la);
    size_t off = la;
    if (need_sep) {
        out[off++] = sol_platform_path_separator();
    }
    memcpy(out + off, b, lb);
    out[off + lb] = '\0';
    return out;
}

const char *sol_platform_basename(const char *path)
{
    if (!path || *path == '\0') {
        return "";
    }

    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (sol_platform_is_path_separator(*p)) {
            base = p + 1;
        }
    }
    return base;
}

bool sol_platform_get_cwd(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0u) {
        return false;
    }

#if defined(_WIN32)
    return _getcwd(buffer, (int)buffer_size) != NULL;
#else
    return getcwd(buffer, buffer_size) != NULL;
#endif
}

bool sol_platform_get_executable_path(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0u) {
        return false;
    }
    buffer[0] = '\0';

#if defined(_WIN32)
    const DWORD len = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (len == 0u || len >= buffer_size) {
        buffer[0] = '\0';
        return false;
    }
    return true;
#elif defined(__APPLE__)
    if (buffer_size > UINT32_MAX) {
        buffer_size = UINT32_MAX;
    }
    uint32_t len = (uint32_t)buffer_size;
    if (_NSGetExecutablePath(buffer, &len) != 0) {
        buffer[0] = '\0';
        return false;
    }
    buffer[buffer_size - 1u] = '\0';
    return true;
#else
    const ssize_t len = readlink("/proc/self/exe", buffer, buffer_size - 1u);
    if (len <= 0 || (size_t)len >= buffer_size) {
        buffer[0] = '\0';
        return false;
    }
    buffer[len] = '\0';
    return true;
#endif
}

char *sol_platform_config_home_dir(void)
{
#if defined(_WIN32)
    const char *home = getenv("APPDATA");
    if (!home || *home == '\0') {
        home = getenv("USERPROFILE");
    }
    const char *name = "sol";
#else
    const char *home = getenv("HOME");
    const char *name = ".sol";
#endif

    if (!home || *home == '\0') {
        return NULL;
    }

    return sol_platform_path_join(home, name);
}

bool sol_platform_get_path_info(const char *path, SolPathInfo *out_info)
{
    if (!path || !out_info) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));

#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return false;
    }

    out_info->exists = true;
    out_info->is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    out_info->is_regular_file = !out_info->is_directory;
    out_info->size_bytes = ((uint64_t)data.nFileSizeHigh << 32u) | (uint64_t)data.nFileSizeLow;
    return true;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    out_info->exists = true;
    out_info->is_directory = S_ISDIR(st.st_mode);
    out_info->is_regular_file = S_ISREG(st.st_mode);
    out_info->size_bytes = (uint64_t)st.st_size;
    return true;
#endif
}

bool sol_platform_mkdir_p(const char *path)
{
    if (!path || *path == '\0') {
        return false;
    }

#if defined(_WIN32)
#define SOL_MKDIR(path_expr) _mkdir(path_expr)
#else
#define SOL_MKDIR(path_expr) mkdir((path_expr), 0755)
#endif

    if (SOL_MKDIR(path) == 0 || errno == EEXIST) {
        return true;
    }

    if (errno != ENOENT) {
        return false;
    }

    char *copy = (char *)malloc(strlen(path) + 1u);
    if (!copy) {
        return false;
    }
    strcpy(copy, path);

    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (!sol_platform_is_path_separator(*p)) {
            continue;
        }

        const char saved = *p;
        *p = '\0';
        if (SOL_MKDIR(copy) != 0 && errno != EEXIST) {
            free(copy);
            return false;
        }
        *p = saved;
    }

    const bool ok = (SOL_MKDIR(copy) == 0 || errno == EEXIST);
    free(copy);
    return ok;
#undef SOL_MKDIR
}

bool sol_platform_create_empty_file(const char *path, bool fail_if_exists)
{
    if (!path || *path == '\0') {
        return false;
    }

#if defined(_WIN32)
    DWORD disposition = fail_if_exists ? CREATE_NEW : OPEN_ALWAYS;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, disposition,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
#else
    int flags = O_WRONLY | O_CREAT;
    if (fail_if_exists) {
        flags |= O_EXCL;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
#endif
}

static bool sol_platform_is_root_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return true;
    }
    if (path[1] == '\0' && sol_platform_is_path_separator(path[0])) {
        return true;
    }
#if defined(_WIN32)
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' &&
        (path[2] == '\0' ||
         (sol_platform_is_path_separator(path[2]) && path[3] == '\0'))) {
        return true;
    }
#endif
    return false;
}

bool sol_platform_remove_path_recursive(const char *path)
{
    if (sol_platform_is_root_path(path)) {
        return false;
    }

    SolPathInfo info;
    if (!sol_platform_get_path_info(path, &info)) {
        return false;
    }

    if (!info.is_directory) {
#if defined(_WIN32)
        return DeleteFileA(path) != 0;
#else
        return unlink(path) == 0;
#endif
    }

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, path)) {
        return false;
    }

    bool ok = true;
    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        char *child = sol_platform_path_join(path, entry.name);
        if (!child) {
            ok = false;
            continue;
        }
        if (!sol_platform_remove_path_recursive(child)) {
            ok = false;
        }
        free(child);
    }
    sol_platform_dir_close(&iter);
    if (!ok) {
        return false;
    }

#if defined(_WIN32)
    return RemoveDirectoryA(path) != 0;
#else
    return rmdir(path) == 0;
#endif
}

static bool sol_platform_copy_file(const char *source_path, const char *dest_path)
{
    FILE *src = fopen(source_path, "rb");
    if (!src) {
        return false;
    }

    FILE *dst = fopen(dest_path, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }

    bool ok = true;
    unsigned char buffer[64 * 1024];
    for (;;) {
        size_t n = fread(buffer, 1u, sizeof(buffer), src);
        if (n > 0u && fwrite(buffer, 1u, n, dst) != n) {
            ok = false;
            break;
        }
        if (n < sizeof(buffer)) {
            if (ferror(src)) {
                ok = false;
            }
            break;
        }
    }

    if (fclose(dst) != 0) {
        ok = false;
    }
    fclose(src);
    if (!ok) {
#if defined(_WIN32)
        DeleteFileA(dest_path);
#else
        unlink(dest_path);
#endif
    }
    return ok;
}

bool sol_platform_copy_path_recursive(const char *source_path, const char *dest_path)
{
    if (!source_path || !dest_path || source_path[0] == '\0' ||
        dest_path[0] == '\0' || strcmp(source_path, dest_path) == 0) {
        return false;
    }

    SolPathInfo info;
    if (!sol_platform_get_path_info(source_path, &info)) {
        return false;
    }

    if (!info.is_directory) {
        return sol_platform_copy_file(source_path, dest_path);
    }

    if (!sol_platform_mkdir_p(dest_path)) {
        return false;
    }

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, source_path)) {
        return false;
    }

    bool ok = true;
    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        char *child_src = sol_platform_path_join(source_path, entry.name);
        char *child_dst = sol_platform_path_join(dest_path, entry.name);
        if (!child_src || !child_dst ||
            !sol_platform_copy_path_recursive(child_src, child_dst)) {
            ok = false;
        }
        free(child_src);
        free(child_dst);
    }
    sol_platform_dir_close(&iter);
    if (!ok) {
        (void)sol_platform_remove_path_recursive(dest_path);
    }
    return ok;
}

bool sol_platform_move_path(const char *source_path, const char *dest_path)
{
    if (!source_path || !dest_path || source_path[0] == '\0' ||
        dest_path[0] == '\0' || strcmp(source_path, dest_path) == 0) {
        return false;
    }

    if (rename(source_path, dest_path) == 0) {
        return true;
    }

    if (!sol_platform_copy_path_recursive(source_path, dest_path)) {
        return false;
    }
    if (!sol_platform_remove_path_recursive(source_path)) {
        (void)sol_platform_remove_path_recursive(dest_path);
        return false;
    }
    return true;
}

uint32_t sol_platform_cpu_count(void)
{
#if defined(_WIN32)
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0u) {
        return 1u;
    }
    return (uint32_t)count;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        return 1u;
    }
    return (uint32_t)count;
#endif
}

uint64_t sol_platform_now_monotonic_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ll) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#if defined(_WIN32)
static void sol_platform_set_dl_error(void)
{
    DWORD err = GetLastError();
    if (err == 0u) {
        g_sol_dl_error[0] = '\0';
        return;
    }

    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        0,
        g_sol_dl_error,
        (DWORD)sizeof(g_sol_dl_error),
        NULL
    );

    if (size == 0u) {
        snprintf(g_sol_dl_error, sizeof(g_sol_dl_error), "Windows error %lu", (unsigned long)err);
    }
}
#endif

void *sol_platform_library_open(const char *path)
{
#if defined(_WIN32)
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        sol_platform_set_dl_error();
    } else {
        g_sol_dl_error[0] = '\0';
    }
    return (void *)module;
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *sol_platform_library_symbol(void *library, const char *symbol)
{
#if defined(_WIN32)
    FARPROC proc = GetProcAddress((HMODULE)library, symbol);
    if (!proc) {
        sol_platform_set_dl_error();
    } else {
        g_sol_dl_error[0] = '\0';
    }
    return (void *)proc;
#else
    return dlsym(library, symbol);
#endif
}

bool sol_platform_library_close(void *library)
{
#if defined(_WIN32)
    if (FreeLibrary((HMODULE)library)) {
        return true;
    }
    sol_platform_set_dl_error();
    return false;
#else
    return dlclose(library) == 0;
#endif
}

const char *sol_platform_library_last_error(void)
{
#if defined(_WIN32)
    return g_sol_dl_error[0] != '\0' ? g_sol_dl_error : NULL;
#else
    return dlerror();
#endif
}

const char *sol_platform_dynamic_library_extension(void)
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

bool sol_platform_dir_open(SolDirectoryIter *iter, const char *path)
{
    if (!iter || !path) {
        return false;
    }

    memset(iter, 0, sizeof(*iter));
    iter->base_path = (char *)malloc(strlen(path) + 1u);
    if (!iter->base_path) {
        return false;
    }
    strcpy(iter->base_path, path);

#if defined(_WIN32)
    char *pattern = sol_platform_path_join(path, "*");
    if (!pattern) {
        free(iter->base_path);
        iter->base_path = NULL;
        return false;
    }

    WIN32_FIND_DATAA find_data;
    HANDLE handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        free(iter->base_path);
        iter->base_path = NULL;
        return false;
    }

    iter->handle = (void *)handle;
    sol_platform_copy_find_data(iter, &find_data);
    iter->has_pending = true;
#else
    DIR *dir = opendir(path);
    if (!dir) {
        free(iter->base_path);
        iter->base_path = NULL;
        return false;
    }

    iter->dir = (void *)dir;
#endif

    return true;
}

bool sol_platform_dir_next(SolDirectoryIter *iter, SolDirectoryEntry *entry)
{
    if (!iter || !entry) {
        return false;
    }

#if defined(_WIN32)
    HANDLE handle = (HANDLE)iter->handle;
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    WIN32_FIND_DATAA data;
    for (;;) {
        if (iter->has_pending) {
            data.dwFileAttributes = iter->find_data.dwFileAttributes;
            strncpy(data.cFileName, iter->find_data.cFileName, sizeof(data.cFileName) - 1u);
            data.cFileName[sizeof(data.cFileName) - 1u] = '\0';
            iter->has_pending = false;
        } else {
            if (!FindNextFileA(handle, &data)) {
                return false;
            }
        }

        const char *name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        sol_platform_copy_find_data(iter, &data);
        entry->name = iter->find_data.cFileName;
        entry->is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
        return true;
    }
#else
    DIR *dir = (DIR *)iter->dir;
    if (!dir) {
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        entry->name = ent->d_name;
#if defined(DT_DIR)
        if (ent->d_type == DT_DIR) {
            entry->is_directory = true;
            return true;
        }
        if (ent->d_type != DT_UNKNOWN) {
            entry->is_directory = false;
            return true;
        }
#endif

        char *full = sol_platform_path_join(iter->base_path, ent->d_name);
        if (!full) {
            continue;
        }

        SolPathInfo info;
        entry->is_directory = sol_platform_get_path_info(full, &info) && info.is_directory;
        free(full);
        return true;
    }

    return false;
#endif
}

void sol_platform_dir_close(SolDirectoryIter *iter)
{
    if (!iter) {
        return;
    }

#if defined(_WIN32)
    HANDLE handle = (HANDLE)iter->handle;
    if (handle && handle != INVALID_HANDLE_VALUE) {
        FindClose(handle);
    }
#else
    DIR *dir = (DIR *)iter->dir;
    if (dir) {
        closedir(dir);
    }
#endif

    free(iter->base_path);
    memset(iter, 0, sizeof(*iter));
}

bool sol_platform_map_file_readonly(const char *path, SolMappedFile *out_file, const char **out_error)
{
    if (out_error) {
        *out_error = NULL;
    }

    if (!path || !out_file) {
        if (out_error) {
            *out_error = "invalid arguments";
        }
        return false;
    }

    memset(out_file, 0, sizeof(*out_file));

#if defined(_WIN32)
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (out_error) {
            *out_error = "CreateFileA() failed";
        }
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        if (out_error) {
            *out_error = "GetFileSizeEx() failed";
        }
        return false;
    }

    if (size.QuadPart <= 0) {
        out_file->file_handle = (void *)file;
        out_file->mapping_handle = NULL;
        out_file->data = NULL;
        out_file->size_bytes = 0u;
        return true;
    }

    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        if (out_error) {
            *out_error = "CreateFileMappingA() failed";
        }
        return false;
    }

    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        if (out_error) {
            *out_error = "MapViewOfFile() failed";
        }
        return false;
    }

    out_file->file_handle = (void *)file;
    out_file->mapping_handle = (void *)mapping;
    out_file->data = (const uint8_t *)view;
    out_file->size_bytes = (size_t)size.QuadPart;
    return true;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (out_error) {
            *out_error = "open() failed";
        }
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (out_error) {
            *out_error = "fstat() failed";
        }
        return false;
    }

    if (st.st_size <= 0) {
        close(fd);
        out_file->data = NULL;
        out_file->size_bytes = 0u;
        out_file->mapping_base = NULL;
        return true;
    }

    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        if (out_error) {
            *out_error = "mmap() failed";
        }
        return false;
    }

    out_file->mapping_base = map;
    out_file->data = (const uint8_t *)map;
    out_file->size_bytes = (size_t)st.st_size;
    return true;
#endif
}

void sol_platform_unmap_file(SolMappedFile *mapped_file)
{
    if (!mapped_file) {
        return;
    }

#if defined(_WIN32)
    if (mapped_file->data) {
        UnmapViewOfFile((const void *)mapped_file->data);
    }
    if (mapped_file->mapping_handle) {
        CloseHandle((HANDLE)mapped_file->mapping_handle);
    }
    if (mapped_file->file_handle) {
        CloseHandle((HANDLE)mapped_file->file_handle);
    }
#else
    if (mapped_file->mapping_base && mapped_file->size_bytes > 0u) {
        munmap(mapped_file->mapping_base, mapped_file->size_bytes);
    }
#endif

    memset(mapped_file, 0, sizeof(*mapped_file));
}
