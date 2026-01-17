/*
 * Sol IDE - Path Utilities Implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef SOL_PLATFORM_WINDOWS
    #include <windows.h>
    #include <shlobj.h>
    #include <direct.h>
    #define PATH_SEP '\\'
#else
    #include <unistd.h>
    #include <pwd.h>
    #define PATH_SEP '/'
#endif

char* sol_path_join(const char* base, const char* path) {
    if (!base || !path) return NULL;
    
    size_t base_len = strlen(base);
    size_t path_len = strlen(path);
    
    /* Remove trailing separator from base */
    while (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\')) {
        base_len--;
    }
    
    /* Remove leading separator from path */
    while (*path == '/' || *path == '\\') {
        path++;
        path_len--;
    }
    
    char* result = (char*)malloc(base_len + 1 + path_len + 1);
    if (!result) return NULL;
    
    memcpy(result, base, base_len);
    result[base_len] = PATH_SEP;
    memcpy(result + base_len + 1, path, path_len);
    result[base_len + 1 + path_len] = '\0';
    
    return result;
}

char* sol_path_dirname(const char* path) {
    if (!path) return NULL;
    
    const char* last_sep = NULL;
    const char* p = path;
    
    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
        p++;
    }
    
    if (!last_sep) {
        return sol_strdup(".");
    }
    
    size_t len = last_sep - path;
    if (len == 0) len = 1;  /* Root directory */
    
    return sol_strndup(path, len);
}

const char* sol_path_basename(const char* path) {
    if (!path) return NULL;
    
    const char* last_sep = path;
    const char* p = path;
    
    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_sep = p + 1;
        }
        p++;
    }
    
    return last_sep;
}

const char* sol_path_extension(const char* path) {
    const char* basename = sol_path_basename(path);
    if (!basename) return NULL;
    
    const char* dot = NULL;
    while (*basename) {
        if (*basename == '.') {
            dot = basename;
        }
        basename++;
    }
    
    return dot ? dot + 1 : NULL;
}

char* sol_path_normalize(const char* path) {
    if (!path) return NULL;
    
    size_t len = strlen(path);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    char* dst = result;
    const char* src = path;
    
    while (*src) {
        if (*src == '/' || *src == '\\') {
            /* Normalize separators */
            *dst++ = PATH_SEP;
            /* Skip consecutive separators */
            while (src[1] == '/' || src[1] == '\\') {
                src++;
            }
        } else {
            *dst++ = *src;
        }
        src++;
    }
    
    /* Remove trailing separator (unless root) */
    if (dst > result + 1 && dst[-1] == PATH_SEP) {
        dst--;
    }
    
    *dst = '\0';
    return result;
}

bool sol_path_exists(const char* path) {
    if (!path) return false;
    
#ifdef SOL_PLATFORM_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

bool sol_path_is_directory(const char* path) {
    if (!path) return false;
    
#ifdef SOL_PLATFORM_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool sol_path_is_file(const char* path) {
    if (!path) return false;
    
#ifdef SOL_PLATFORM_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

char* sol_path_home(void) {
#ifdef SOL_PLATFORM_WINDOWS
    char* home = (char*)malloc(MAX_PATH);
    if (!home) return NULL;
    
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, home) == S_OK) {
        return home;
    }
    
    /* Fallback to USERPROFILE */
    const char* userprofile = getenv("USERPROFILE");
    if (userprofile) {
        strcpy(home, userprofile);
        return home;
    }
    
    free(home);
    return NULL;
#else
    const char* home = getenv("HOME");
    if (home) {
        return sol_strdup(home);
    }
    
    struct passwd* pw = getpwuid(getuid());
    if (pw) {
        return sol_strdup(pw->pw_dir);
    }
    
    return NULL;
#endif
}

char* sol_path_config_dir(void) {
#ifdef SOL_PLATFORM_WINDOWS
    char* config = (char*)malloc(MAX_PATH);
    if (!config) return NULL;
    
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, config) == S_OK) {
        char* sol_dir = sol_path_join(config, "sol");
        free(config);
        return sol_dir;
    }
    
    free(config);
    return NULL;
#elif defined(SOL_PLATFORM_MACOS)
    char* home = sol_path_home();
    if (!home) return NULL;
    
    char* config = sol_path_join(home, "Library/Application Support/sol");
    free(home);
    return config;
#else
    /* XDG Base Directory Specification */
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config) {
        return sol_path_join(xdg_config, "sol");
    }
    
    char* home = sol_path_home();
    if (!home) return NULL;
    
    char* config = sol_path_join(home, ".config/sol");
    free(home);
    return config;
#endif
}
