// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_theme.h — Runtime CSS theme registry. */

#ifndef SOL_THEME_H
#define SOL_THEME_H

#include <stdbool.h>
#include <stddef.h>

#define SOL_THEME_ID_MAX 63u
#define SOL_THEME_NAME_MAX 63u
#define SOL_THEME_MAX 64u

typedef struct SolThemeRegistry SolThemeRegistry;
typedef void (*SolThemeChangeFn)(void *user_data);

typedef struct SolThemeDesc {
    const char *id;
    const char *name;
    const char *base_id;
    const char *css;
} SolThemeDesc;

/* Create an empty registry. */
SolThemeRegistry *sol_theme_registry_create(void);

/* Destroy a registry and every copied descriptor string it owns. */
void sol_theme_registry_destroy(SolThemeRegistry *registry);

/* Register CSS, optionally appending it to a copied registered base theme. */
bool sol_theme_register(SolThemeRegistry *registry, const SolThemeDesc *desc);

/* Remove a theme by id, selecting the first remaining theme when necessary. */
bool sol_theme_unregister(SolThemeRegistry *registry, const char *id);

/* Return the number of registered themes. */
size_t sol_theme_count(const SolThemeRegistry *registry);

/* Return descriptor data for an indexed theme. Registry retains ownership. */
bool sol_theme_get_info(const SolThemeRegistry *registry, size_t index,
                        const char **out_id, const char **out_name);

/* Return a theme's complete CSS source, or NULL when no matching theme exists. */
const char *sol_theme_css(const SolThemeRegistry *registry, const char *id);

/* Select a registered theme and publish a change notification. */
bool sol_theme_set_active(SolThemeRegistry *registry, const char *id);

/* Return the active theme id or NULL when the registry is empty. */
const char *sol_theme_active_id(const SolThemeRegistry *registry);

/* Return the active theme's complete CSS source or NULL. */
const char *sol_theme_active_css(const SolThemeRegistry *registry);

/* Install the single registry-change observer. */
void sol_theme_set_change_callback(SolThemeRegistry *registry,
                                   SolThemeChangeFn callback,
                                   void *user_data);

#endif /* SOL_THEME_H */
