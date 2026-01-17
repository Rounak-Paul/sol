/*
 * Sol IDE - Arena Allocator Implementation
 * 
 * Memory-efficient bump allocator for fast, grouped allocations.
 * Memory is freed all at once when the arena is destroyed or reset.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Alignment for allocations */
#define SOL_ARENA_ALIGNMENT 8

static inline size_t align_up(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

static sol_arena_block* arena_block_create(size_t min_size) {
    size_t block_size = min_size > SOL_ARENA_BLOCK_SIZE ? min_size : SOL_ARENA_BLOCK_SIZE;
    sol_arena_block* block = (sol_arena_block*)malloc(sizeof(sol_arena_block) + block_size);
    if (!block) return NULL;
    
    block->next = NULL;
    block->size = block_size;
    block->used = 0;
    
    return block;
}

sol_arena* sol_arena_create(void) {
    sol_arena* arena = (sol_arena*)malloc(sizeof(sol_arena));
    if (!arena) return NULL;
    
    arena->first = arena_block_create(SOL_ARENA_BLOCK_SIZE);
    if (!arena->first) {
        free(arena);
        return NULL;
    }
    
    arena->current = arena->first;
    arena->total_allocated = SOL_ARENA_BLOCK_SIZE;
    arena->total_used = 0;
    
    return arena;
}

void sol_arena_destroy(sol_arena* arena) {
    if (!arena) return;
    
    sol_arena_block* block = arena->first;
    while (block) {
        sol_arena_block* next = block->next;
        free(block);
        block = next;
    }
    
    free(arena);
}

void* sol_arena_alloc(sol_arena* arena, size_t size) {
    if (!arena || size == 0) return NULL;
    
    size = align_up(size, SOL_ARENA_ALIGNMENT);
    
    /* Check if current block has space */
    if (arena->current->used + size <= arena->current->size) {
        void* ptr = arena->current->data + arena->current->used;
        arena->current->used += size;
        arena->total_used += size;
        return ptr;
    }
    
    /* Need a new block */
    sol_arena_block* new_block = arena_block_create(size);
    if (!new_block) return NULL;
    
    arena->current->next = new_block;
    arena->current = new_block;
    arena->total_allocated += new_block->size;
    
    void* ptr = new_block->data;
    new_block->used = size;
    arena->total_used += size;
    
    return ptr;
}

void* sol_arena_alloc_zero(sol_arena* arena, size_t size) {
    void* ptr = sol_arena_alloc(arena, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

char* sol_arena_strdup(sol_arena* arena, const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = (char*)sol_arena_alloc(arena, len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

char* sol_arena_strndup(sol_arena* arena, const char* str, size_t len) {
    if (!str) return NULL;
    char* copy = (char*)sol_arena_alloc(arena, len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    return copy;
}

void sol_arena_reset(sol_arena* arena) {
    if (!arena) return;
    
    /* Reset all blocks */
    sol_arena_block* block = arena->first;
    while (block) {
        block->used = 0;
        block = block->next;
    }
    
    arena->current = arena->first;
    arena->total_used = 0;
}
