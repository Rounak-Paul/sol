/*
 * Sol IDE - Hash Map Implementation
 * 
 * Open addressing with linear probing.
 * Uses FNV-1a for hashing.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

#define HASHMAP_INITIAL_CAP 16
#define HASHMAP_LOAD_FACTOR 0.75

typedef struct {
    void* key;
    void* value;
    bool occupied;
    bool deleted;
} hashmap_entry;

struct sol_hashmap {
    hashmap_entry* entries;
    size_t capacity;
    size_t count;
    size_t key_size;
    size_t value_size;
    bool string_keys;
};

/* FNV-1a hash */
static uint64_t fnv1a_hash(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint64_t hash = 14695981039346656037ULL;
    
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    
    return hash;
}

static uint64_t hash_key(sol_hashmap* map, const void* key) {
    if (map->string_keys) {
        return fnv1a_hash(key, strlen((const char*)key));
    }
    return fnv1a_hash(key, map->key_size);
}

static bool keys_equal(sol_hashmap* map, const void* a, const void* b) {
    if (map->string_keys) {
        return strcmp((const char*)a, (const char*)b) == 0;
    }
    return memcmp(a, b, map->key_size) == 0;
}

sol_hashmap* sol_hashmap_create(size_t key_size, size_t value_size) {
    sol_hashmap* map = (sol_hashmap*)calloc(1, sizeof(sol_hashmap));
    if (!map) return NULL;
    
    map->capacity = HASHMAP_INITIAL_CAP;
    map->count = 0;
    map->key_size = key_size;
    map->value_size = value_size;
    map->string_keys = false;
    
    map->entries = (hashmap_entry*)calloc(map->capacity, sizeof(hashmap_entry));
    if (!map->entries) {
        free(map);
        return NULL;
    }
    
    return map;
}

sol_hashmap* sol_hashmap_create_string(size_t value_size) {
    sol_hashmap* map = sol_hashmap_create(sizeof(char*), value_size);
    if (map) {
        map->string_keys = true;
    }
    return map;
}

void sol_hashmap_destroy(sol_hashmap* map) {
    if (!map) return;
    
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            if (map->string_keys && map->entries[i].key) {
                free(map->entries[i].key);
            }
            if (map->entries[i].value) {
                free(map->entries[i].value);
            }
        }
    }
    
    free(map->entries);
    free(map);
}

static bool hashmap_resize(sol_hashmap* map, size_t new_cap) {
    hashmap_entry* old_entries = map->entries;
    size_t old_cap = map->capacity;
    
    map->entries = (hashmap_entry*)calloc(new_cap, sizeof(hashmap_entry));
    if (!map->entries) {
        map->entries = old_entries;
        return false;
    }
    
    map->capacity = new_cap;
    map->count = 0;
    
    for (size_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            /* Reinsert into new table */
            uint64_t hash = hash_key(map, old_entries[i].key);
            size_t idx = hash % new_cap;
            
            while (map->entries[idx].occupied) {
                idx = (idx + 1) % new_cap;
            }
            
            map->entries[idx] = old_entries[i];
            map->count++;
        } else if (old_entries[i].occupied) {
            /* Was deleted, clean up */
            if (map->string_keys && old_entries[i].key) {
                free(old_entries[i].key);
            }
            if (old_entries[i].value) {
                free(old_entries[i].value);
            }
        }
    }
    
    free(old_entries);
    return true;
}

bool sol_hashmap_set(sol_hashmap* map, const void* key, const void* value) {
    if (!map || !key) return false;
    
    /* Resize if needed */
    if ((float)(map->count + 1) / map->capacity > HASHMAP_LOAD_FACTOR) {
        if (!hashmap_resize(map, map->capacity * 2)) {
            return false;
        }
    }
    
    uint64_t hash = hash_key(map, key);
    size_t idx = hash % map->capacity;
    
    /* Find slot */
    while (map->entries[idx].occupied && !map->entries[idx].deleted) {
        if (keys_equal(map, map->entries[idx].key, key)) {
            /* Update existing */
            memcpy(map->entries[idx].value, value, map->value_size);
            return true;
        }
        idx = (idx + 1) % map->capacity;
    }
    
    /* Insert new */
    hashmap_entry* entry = &map->entries[idx];
    
    if (map->string_keys) {
        entry->key = sol_strdup((const char*)key);
    } else {
        entry->key = malloc(map->key_size);
        if (entry->key) {
            memcpy(entry->key, key, map->key_size);
        }
    }
    
    entry->value = malloc(map->value_size);
    if (entry->value) {
        memcpy(entry->value, value, map->value_size);
    }
    
    entry->occupied = true;
    entry->deleted = false;
    map->count++;
    
    return true;
}

void* sol_hashmap_get(sol_hashmap* map, const void* key) {
    if (!map || !key) return NULL;
    
    uint64_t hash = hash_key(map, key);
    size_t idx = hash % map->capacity;
    size_t start_idx = idx;
    
    do {
        if (!map->entries[idx].occupied) {
            return NULL;
        }
        if (!map->entries[idx].deleted && keys_equal(map, map->entries[idx].key, key)) {
            return map->entries[idx].value;
        }
        idx = (idx + 1) % map->capacity;
    } while (idx != start_idx);
    
    return NULL;
}

bool sol_hashmap_remove(sol_hashmap* map, const void* key) {
    if (!map || !key) return false;
    
    uint64_t hash = hash_key(map, key);
    size_t idx = hash % map->capacity;
    size_t start_idx = idx;
    
    do {
        if (!map->entries[idx].occupied) {
            return false;
        }
        if (!map->entries[idx].deleted && keys_equal(map, map->entries[idx].key, key)) {
            map->entries[idx].deleted = true;
            map->count--;
            return true;
        }
        idx = (idx + 1) % map->capacity;
    } while (idx != start_idx);
    
    return false;
}

size_t sol_hashmap_count(sol_hashmap* map) {
    return map ? map->count : 0;
}

void sol_hashmap_clear(sol_hashmap* map) {
    if (!map) return;
    
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            if (map->string_keys && map->entries[i].key) {
                free(map->entries[i].key);
            }
            if (map->entries[i].value) {
                free(map->entries[i].value);
            }
            map->entries[i].occupied = false;
            map->entries[i].deleted = false;
        }
    }
    map->count = 0;
}

bool sol_hashmap_set_string(sol_hashmap* map, const char* key, const void* value) {
    return sol_hashmap_set(map, key, value);
}

void* sol_hashmap_get_string(sol_hashmap* map, const char* key) {
    return sol_hashmap_get(map, key);
}
