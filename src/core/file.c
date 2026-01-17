/*
 * Sol IDE - File I/O Implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef SOL_PLATFORM_WINDOWS
    #include <windows.h>
    #include <io.h>
#else
    #include <unistd.h>
    #include <dirent.h>
    #include <errno.h>
#endif

char* sol_file_read(const char* path, size_t* out_len) {
    if (!path) return NULL;
    
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    
    char* data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    
    size_t read = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    
    data[read] = '\0';
    
    if (out_len) {
        *out_len = read;
    }
    
    return data;
}

sol_result sol_file_write(const char* path, const char* data, size_t len) {
    if (!path || !data) return SOL_ERROR_INVALID_ARG;
    
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        return SOL_ERROR_IO;
    }
    
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    
    if (written != len) {
        return SOL_ERROR_IO;
    }
    
    return SOL_OK;
}

uint64_t sol_file_mtime(const char* path) {
    if (!path) return 0;
    
#ifdef SOL_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER time;
        time.LowPart = data.ftLastWriteTime.dwLowDateTime;
        time.HighPart = data.ftLastWriteTime.dwHighDateTime;
        return time.QuadPart;
    }
    return 0;
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        return (uint64_t)st.st_mtime;
    }
    return 0;
#endif
}

sol_array(char*) sol_dir_list(const char* path) {
    if (!path) return NULL;
    
    sol_array(char*) list = NULL;
    
#ifdef SOL_PLATFORM_WINDOWS
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    
    WIN32_FIND_DATAA find_data;
    HANDLE handle = FindFirstFileA(pattern, &find_data);
    
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    
    do {
        /* Skip . and .. */
        if (strcmp(find_data.cFileName, ".") == 0 ||
            strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        
        char* name = sol_strdup(find_data.cFileName);
        if (name) {
            /* Append / for directories */
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                size_t len = strlen(name);
                char* dir_name = (char*)realloc(name, len + 2);
                if (dir_name) {
                    name = dir_name;
                    name[len] = '/';
                    name[len + 1] = '\0';
                }
            }
            sol_array_push(list, name);
        }
    } while (FindNextFileA(handle, &find_data));
    
    FindClose(handle);
#else
    DIR* dir = opendir(path);
    if (!dir) return NULL;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char* name;
        bool is_dir = false;
        
        /* Check if directory */
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            /* d_type not available, use stat */
            char* full_path = sol_path_join(path, entry->d_name);
            if (full_path) {
                is_dir = sol_path_is_directory(full_path);
                free(full_path);
            }
        }
        
        if (is_dir) {
            size_t len = strlen(entry->d_name);
            name = (char*)malloc(len + 2);
            if (name) {
                memcpy(name, entry->d_name, len);
                name[len] = '/';
                name[len + 1] = '\0';
            }
        } else {
            name = sol_strdup(entry->d_name);
        }
        
        if (name) {
            sol_array_push(list, name);
        }
    }
    
    closedir(dir);
#endif
    
    return list;
}
