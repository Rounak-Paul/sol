/*
 * Sol IDE - Dynamic Array Implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

void* sol_array_grow(void* arr, size_t elem_size, size_t min_cap) {
    sol_array_header* header;
    size_t new_cap;
    size_t old_cap;
    
    if (arr) {
        header = sol_array_header(arr);
        old_cap = header->capacity;
        
        /* Double capacity or use min_cap, whichever is larger */
        new_cap = old_cap * 2;
        if (new_cap < min_cap) new_cap = min_cap;
        if (new_cap < SOL_ARRAY_INITIAL_CAP) new_cap = SOL_ARRAY_INITIAL_CAP;
        
        header = (sol_array_header*)realloc(header, sizeof(sol_array_header) + new_cap * elem_size);
        if (!header) return NULL;
        
        header->capacity = new_cap;
        header->elem_size = elem_size;
    } else {
        /* First allocation */
        new_cap = min_cap > SOL_ARRAY_INITIAL_CAP ? min_cap : SOL_ARRAY_INITIAL_CAP;
        
        header = (sol_array_header*)malloc(sizeof(sol_array_header) + new_cap * elem_size);
        if (!header) return NULL;
        
        header->count = 0;
        header->capacity = new_cap;
        header->elem_size = elem_size;
        header->data = NULL;  /* Not used, but initialize anyway */
    }
    
    return (char*)header + sizeof(sol_array_header);
}

void sol_array_free_impl(void* arr) {
    if (arr) {
        free(sol_array_header(arr));
    }
}
