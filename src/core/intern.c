/*
 * Sol IDE - String Interning Implementation
 * 
 * For frequently used strings like file paths, command names, etc.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

#define INTERN_INITIAL_CAP 256

struct sol_intern {
    sol_hashmap* strings;
    sol_arena* arena;
};

sol_intern* sol_intern_create(void) {
    sol_intern* intern = (sol_intern*)malloc(sizeof(sol_intern));
    if (!intern) return NULL;
    
    intern->strings = sol_hashmap_create_string(sizeof(const char*));
    if (!intern->strings) {
        free(intern);
        return NULL;
    }
    
    intern->arena = sol_arena_create();
    if (!intern->arena) {
        sol_hashmap_destroy(intern->strings);
        free(intern);
        return NULL;
    }
    
    return intern;
}

void sol_intern_destroy(sol_intern* intern) {
    if (!intern) return;
    
    sol_hashmap_destroy(intern->strings);
    sol_arena_destroy(intern->arena);
    free(intern);
}

const char* sol_intern_string(sol_intern* intern, const char* str) {
    if (!intern || !str) return NULL;
    
    /* Check if already interned */
    const char** existing = (const char**)sol_hashmap_get_string(intern->strings, str);
    if (existing) {
        return *existing;
    }
    
    /* Intern new string */
    const char* interned = sol_arena_strdup(intern->arena, str);
    if (!interned) return NULL;
    
    sol_hashmap_set_string(intern->strings, interned, &interned);
    return interned;
}

const char* sol_intern_stringn(sol_intern* intern, const char* str, size_t len) {
    if (!intern || !str) return NULL;
    
    /* Create temporary null-terminated string for lookup */
    char* temp = (char*)malloc(len + 1);
    if (!temp) return NULL;
    
    memcpy(temp, str, len);
    temp[len] = '\0';
    
    const char* result = sol_intern_string(intern, temp);
    free(temp);
    
    return result;
}
