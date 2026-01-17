/*
 * Sol IDE - Configuration System
 * Stub implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Config system stubs */

sol_config* sol_config_create(void) {
    sol_config* cfg = calloc(1, sizeof(sol_config));
    if (!cfg) return NULL;
    
    cfg->values = sol_hashmap_create(sizeof(char*), sizeof(sol_config_value*));
    cfg->path = NULL;
    cfg->modified = false;
    
    return cfg;
}

void sol_config_load_file(sol_config* cfg, const char* path) {
    if (!cfg || !path) return;
    /* TODO: Parse JSON config file */
    cfg->path = strdup(path);
}

sol_config* sol_config_load(const char* path) {
    sol_config* cfg = sol_config_create();
    if (cfg && path) {
        sol_config_load_file(cfg, path);
    }
    return cfg;
}

void sol_config_destroy(sol_config* cfg) {
    if (!cfg) return;
    
    if (cfg->values) {
        sol_hashmap_destroy(cfg->values);
    }
    free(cfg->path);
    free(cfg);
}

sol_result sol_config_save(sol_config* cfg) {
    if (!cfg || !cfg->path) return SOL_ERROR_INVALID_ARG;
    /* TODO: Write JSON config file */
    cfg->modified = false;
    return SOL_OK;
}

sol_config_value* sol_config_get(sol_config* cfg, const char* key) {
    if (!cfg || !key || !cfg->values) return NULL;
    
    sol_config_value** val = sol_hashmap_get(cfg->values, &key);
    return val ? *val : NULL;
}

void sol_config_set(sol_config* cfg, const char* key, sol_config_value* value) {
    if (!cfg || !key || !cfg->values) return;
    
    sol_hashmap_set(cfg->values, &key, &value);
    cfg->modified = true;
}

const char* sol_config_get_string(sol_config* cfg, const char* key, const char* default_val) {
    sol_config_value* val = sol_config_get(cfg, key);
    if (val && val->type == SOL_CONFIG_STRING) {
        return val->data.string;
    }
    return default_val;
}

int64_t sol_config_get_int(sol_config* cfg, const char* key, int64_t default_val) {
    sol_config_value* val = sol_config_get(cfg, key);
    if (val && val->type == SOL_CONFIG_INT) {
        return val->data.integer;
    }
    return default_val;
}

bool sol_config_get_bool(sol_config* cfg, const char* key, bool default_val) {
    sol_config_value* val = sol_config_get(cfg, key);
    if (val && val->type == SOL_CONFIG_BOOL) {
        return val->data.boolean;
    }
    return default_val;
}

int sol_config_get_tab_width(sol_config* cfg) {
    return (int)sol_config_get_int(cfg, "editor.tabWidth", 4);
}

sol_theme* sol_config_get_theme(sol_config* cfg) {
    (void)cfg;
    /* Return default theme - will be implemented properly later */
    static sol_theme default_theme = {0};
    return &default_theme;
}

void sol_config_add_recent_file(sol_config* cfg, const char* path) {
    (void)cfg;
    (void)path;
    /* TODO: Add to recent files list */
}
