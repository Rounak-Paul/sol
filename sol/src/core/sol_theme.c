// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_theme.c — Owned runtime CSS theme registry. */

#include "sol_theme.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct SolThemeEntry {
    char *id;
    char *name;
    char *css;
    SolThemeColors colors;
} SolThemeEntry;

struct SolThemeRegistry {
    SolThemeEntry entries[SOL_THEME_MAX];
    size_t count;
    size_t active_index;
    SolThemeChangeFn change_callback;
    void *change_data;
};

/* Duplicate a null-terminated string. */
static char *sol_theme_strdup(const char *text)
{
    if (!text) return NULL;
    const size_t size = strlen(text) + 1u;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

/* Return the index for an id, or SIZE_MAX when absent. */
static size_t sol_theme_find(const SolThemeRegistry *registry, const char *id)
{
    if (!registry || !id) return SIZE_MAX;
    for (size_t i = 0u; i < registry->count; ++i) {
        if (strcmp(registry->entries[i].id, id) == 0) return i;
    }
    return SIZE_MAX;
}

/* Notify the registry observer after externally visible state changes. */
static void sol_theme_notify(SolThemeRegistry *registry)
{
    if (registry && registry->change_callback)
        registry->change_callback(registry->change_data);
}

SolThemeRegistry *sol_theme_registry_create(void)
{
    return (SolThemeRegistry *)calloc(1u, sizeof(SolThemeRegistry));
}

void sol_theme_registry_destroy(SolThemeRegistry *registry)
{
    if (!registry) return;
    for (size_t i = 0u; i < registry->count; ++i) {
        free(registry->entries[i].id);
        free(registry->entries[i].name);
        free(registry->entries[i].css);
    }
    free(registry);
}

bool sol_theme_register(SolThemeRegistry *registry, const SolThemeDesc *desc)
{
    if (!registry || !desc || !desc->id || !desc->name || !desc->css ||
        desc->id[0] == '\0' || desc->name[0] == '\0' || desc->css[0] == '\0' ||
        strlen(desc->id) > SOL_THEME_ID_MAX ||
        strlen(desc->name) > SOL_THEME_NAME_MAX ||
        registry->count >= SOL_THEME_MAX ||
        sol_theme_find(registry, desc->id) != SIZE_MAX) {
        return false;
    }

    const char *base_css = NULL;
    if (desc->base_id && desc->base_id[0] != '\0') {
        const size_t base_index = sol_theme_find(registry, desc->base_id);
        if (base_index == SIZE_MAX || strcmp(desc->base_id, desc->id) == 0)
            return false;
        base_css = registry->entries[base_index].css;
    }

    char *composed_css = NULL;
    if (base_css) {
        const size_t base_size = strlen(base_css);
        const size_t override_size = strlen(desc->css);
        if (base_size > SIZE_MAX - override_size - 2u) return false;
        composed_css = (char *)malloc(base_size + override_size + 2u);
        if (composed_css) {
            memcpy(composed_css, base_css, base_size);
            composed_css[base_size] = '\n';
            memcpy(composed_css + base_size + 1u, desc->css, override_size + 1u);
        }
    } else {
        composed_css = sol_theme_strdup(desc->css);
    }

    SolThemeEntry entry = {
        .id = sol_theme_strdup(desc->id),
        .name = sol_theme_strdup(desc->name),
        .css = composed_css,
        .colors = desc->colors,
    };
    if (base_css && !desc->has_colors) {
        const size_t base_index = sol_theme_find(registry, desc->base_id);
        entry.colors = registry->entries[base_index].colors;
    }
    if (!entry.id || !entry.name || !entry.css) {
        free(entry.id);
        free(entry.name);
        free(entry.css);
        return false;
    }

    registry->entries[registry->count++] = entry;
    if (registry->count == 1u) registry->active_index = 0u;
    sol_theme_notify(registry);
    return true;
}

bool sol_theme_unregister(SolThemeRegistry *registry, const char *id)
{
    const size_t index = sol_theme_find(registry, id);
    if (index == SIZE_MAX) return false;

    const bool removed_active = index == registry->active_index;
    free(registry->entries[index].id);
    free(registry->entries[index].name);
    free(registry->entries[index].css);
    if (index + 1u < registry->count) {
        memmove(&registry->entries[index], &registry->entries[index + 1u],
                (registry->count - index - 1u) * sizeof(registry->entries[0]));
    }
    registry->count--;

    if (registry->count == 0u) {
        registry->active_index = 0u;
    } else if (removed_active) {
        registry->active_index = 0u;
    } else if (index < registry->active_index) {
        registry->active_index--;
    }
    sol_theme_notify(registry);
    return true;
}

size_t sol_theme_count(const SolThemeRegistry *registry)
{
    return registry ? registry->count : 0u;
}

bool sol_theme_get_info(const SolThemeRegistry *registry, size_t index,
                        const char **out_id, const char **out_name)
{
    if (!registry || index >= registry->count) return false;
    if (out_id) *out_id = registry->entries[index].id;
    if (out_name) *out_name = registry->entries[index].name;
    return true;
}

const char *sol_theme_css(const SolThemeRegistry *registry, const char *id)
{
    const size_t index = sol_theme_find(registry, id);
    return index == SIZE_MAX ? NULL : registry->entries[index].css;
}

bool sol_theme_set_active(SolThemeRegistry *registry, const char *id)
{
    const size_t index = sol_theme_find(registry, id);
    if (index == SIZE_MAX) return false;
    if (index == registry->active_index) return true;
    registry->active_index = index;
    sol_theme_notify(registry);
    return true;
}

const char *sol_theme_active_id(const SolThemeRegistry *registry)
{
    return registry && registry->count > 0u
        ? registry->entries[registry->active_index].id : NULL;
}

const char *sol_theme_active_css(const SolThemeRegistry *registry)
{
    return registry && registry->count > 0u
        ? registry->entries[registry->active_index].css : NULL;
}

bool sol_theme_active_colors(const SolThemeRegistry *registry,
                             SolThemeColors *out_colors)
{
    if (!registry || registry->count == 0u || !out_colors) return false;
    *out_colors = registry->entries[registry->active_index].colors;
    return true;
}

void sol_theme_set_change_callback(SolThemeRegistry *registry,
                                   SolThemeChangeFn callback,
                                   void *user_data)
{
    if (!registry) return;
    registry->change_callback = callback;
    registry->change_data = user_data;
}
