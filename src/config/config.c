/*
 * Sol IDE - Configuration System
 * Loads/saves settings from ~/.sol/config.json
 */

#define JSON_H_IMPLEMENTATION
#include "json.h"
#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define PATH_SEP "\\"
#else
#include <unistd.h>
#define PATH_SEP "/"
#endif

/* Get the Sol config directory path (~/.sol) */
static char* get_config_dir(void) {
    const char* home = getenv("HOME");
    if (!home) {
#ifdef _WIN32
        home = getenv("USERPROFILE");
#endif
    }
    if (!home) home = ".";
    
    size_t len = strlen(home) + 6;  /* "/.sol\0" */
    char* path = malloc(len);
    if (!path) return NULL;
    
    snprintf(path, len, "%s" PATH_SEP ".sol", home);
    return path;
}

/* Ensure config directory exists */
static bool ensure_config_dir(void) {
    char* dir = get_config_dir();
    if (!dir) return false;
    
    struct stat st;
    if (stat(dir, &st) != 0) {
        /* Directory doesn't exist, create it */
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            free(dir);
            return false;
        }
    }
    
    free(dir);
    return true;
}

/* Get default config path */
char* sol_config_get_default_path(void) {
    char* dir = get_config_dir();
    if (!dir) return NULL;
    
    size_t len = strlen(dir) + 13;  /* "/config.json\0" */
    char* path = malloc(len);
    if (!path) {
        free(dir);
        return NULL;
    }
    
    snprintf(path, len, "%s" PATH_SEP "config.json", dir);
    free(dir);
    return path;
}

/* Get keybindings config path */
char* sol_config_get_keybindings_path(void) {
    char* dir = get_config_dir();
    if (!dir) return NULL;
    
    size_t len = strlen(dir) + 18;  /* "/keybindings.json\0" */
    char* path = malloc(len);
    if (!path) {
        free(dir);
        return NULL;
    }
    
    snprintf(path, len, "%s" PATH_SEP "keybindings.json", dir);
    free(dir);
    return path;
}

/* Create default config JSON */
static const char* DEFAULT_CONFIG = 
"{\n"
"    \"editor.tabWidth\": 4,\n"
"    \"editor.insertSpaces\": true,\n"
"    \"editor.wordWrap\": false,\n"
"    \"editor.lineNumbers\": true,\n"
"    \"editor.cursorStyle\": \"block\",\n"
"    \"editor.fontSize\": 14,\n"
"    \"editor.fontFamily\": \"monospace\",\n"
"    \"theme\": \"dark\",\n"
"    \"sidebar.visible\": true,\n"
"    \"sidebar.width\": 30,\n"
"    \"terminal.height\": 10,\n"
"    \"files.autoSave\": false,\n"
"    \"files.trimTrailingWhitespace\": true\n"
"}\n";

/* Create default keybindings JSON */
static const char* DEFAULT_KEYBINDINGS =
"{\n"
"    \"flow\": {\n"
"        \"leader\": \"ctrl+space\",\n"
"        \"commands\": {\n"
"            \"o\": \"file.open\",\n"
"            \"O\": \"file.openFolder\",\n"
"            \"n\": \"file.new\",\n"
"            \"s\": \"file.save\",\n"
"            \"S\": \"file.saveAll\",\n"
"            \"w\": \"view.close\",\n"
"            \"x\": \"edit.undo\",\n"
"            \"X\": \"edit.undoAll\",\n"
"            \"r\": \"edit.redo\",\n"
"            \"R\": \"edit.redoAll\",\n"
"            \"c\": \"edit.copy\",\n"
"            \"v\": \"edit.paste\",\n"
"            \"d\": \"edit.cut\",\n"
"            \"f\": \"find.open\",\n"
"            \"g\": \"goto.line\",\n"
"            \"e\": \"view.focusSidebar\",\n"
"            \"p\": \"view.commandPalette\",\n"
"            \"b\": \"view.toggleSidebar\",\n"
"            \"t\": \"view.toggleTerminal\",\n"
"            \"k\": \"view.keybindings\",\n"
"            \"q\": \"app.quit\"\n"
"        }\n"
"    },\n"
"    \"direct\": {\n"
"        \"ctrl+q\": \"app.quit\",\n"
"        \"ctrl+s\": \"file.save\",\n"
"        \"ctrl+z\": \"edit.undo\",\n"
"        \"ctrl+y\": \"edit.redo\"\n"
"    }\n"
"}\n";

/* Write default config files if they don't exist */
static void write_default_configs(void) {
    if (!ensure_config_dir()) return;
    
    char* config_path = sol_config_get_default_path();
    if (config_path) {
        FILE* f = fopen(config_path, "r");
        if (!f) {
            /* File doesn't exist, create it */
            f = fopen(config_path, "w");
            if (f) {
                fputs(DEFAULT_CONFIG, f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
        free(config_path);
    }
    
    char* keybind_path = sol_config_get_keybindings_path();
    if (keybind_path) {
        FILE* f = fopen(keybind_path, "r");
        if (!f) {
            /* File doesn't exist, create it */
            f = fopen(keybind_path, "w");
            if (f) {
                fputs(DEFAULT_KEYBINDINGS, f);
                fclose(f);
            }
        } else {
            fclose(f);
        }
        free(keybind_path);
    }
}

/* Read file contents */
static char* read_file_contents(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    
    char* content = malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    
    size_t read_bytes = fread(content, 1, (size_t)size, f);
    content[read_bytes] = '\0';
    fclose(f);
    
    return content;
}

/* Helper to create config value */
static sol_config_value* config_value_create(sol_config_type type) {
    sol_config_value* val = calloc(1, sizeof(sol_config_value));
    if (val) val->type = type;
    return val;
}

/* Parse JSON value to config value */
static sol_config_value* parse_json_value(struct json_value_s* json_val) {
    if (!json_val) return NULL;
    
    sol_config_value* val = NULL;
    
    switch (json_val->type) {
        case json_type_string: {
            struct json_string_s* str = (struct json_string_s*)json_val->payload;
            val = config_value_create(SOL_CONFIG_STRING);
            if (val) val->data.string = strdup(str->string);
            break;
        }
        case json_type_number: {
            struct json_number_s* num = (struct json_number_s*)json_val->payload;
            val = config_value_create(SOL_CONFIG_INT);
            if (val) {
                /* Check if it's a float */
                if (strchr(num->number, '.')) {
                    val->type = SOL_CONFIG_FLOAT;
                    val->data.number = atof(num->number);
                } else {
                    val->data.integer = atoll(num->number);
                }
            }
            break;
        }
        case json_type_true:
            val = config_value_create(SOL_CONFIG_BOOL);
            if (val) val->data.boolean = true;
            break;
        case json_type_false:
            val = config_value_create(SOL_CONFIG_BOOL);
            if (val) val->data.boolean = false;
            break;
        default:
            break;
    }
    
    return val;
}

/* Recursively parse JSON object and populate config */
static void parse_json_object(sol_config* cfg, struct json_object_s* obj, const char* prefix) {
    if (!cfg || !obj) return;
    
    struct json_object_element_s* elem = obj->start;
    while (elem) {
        /* Build full key with prefix */
        char key[256];
        if (prefix && prefix[0]) {
            snprintf(key, sizeof(key), "%s.%s", prefix, elem->name->string);
        } else {
            strncpy(key, elem->name->string, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
        }
        
        /* Handle nested objects */
        if (elem->value->type == json_type_object) {
            struct json_object_s* nested = (struct json_object_s*)elem->value->payload;
            parse_json_object(cfg, nested, key);
        } else {
            sol_config_value* val = parse_json_value(elem->value);
            if (val) {
                sol_config_set(cfg, key, val);
            }
        }
        
        elem = elem->next;
    }
}

sol_config* sol_config_create(void) {
    sol_config* cfg = calloc(1, sizeof(sol_config));
    if (!cfg) return NULL;
    
    cfg->values = sol_hashmap_create_string(sizeof(sol_config_value*));
    if (!cfg->values) {
        free(cfg);
        return NULL;
    }
    cfg->path = NULL;
    cfg->modified = false;
    
    /* Ensure default config files exist */
    write_default_configs();
    
    return cfg;
}

void sol_config_load_file(sol_config* cfg, const char* path) {
    if (!cfg || !path) return;
    
    char* content = read_file_contents(path);
    if (!content) return;
    
    /* Parse JSON */
    struct json_value_s* root = json_parse(content, strlen(content));
    free(content);
    
    if (!root) return;
    
    if (root->type == json_type_object) {
        struct json_object_s* obj = (struct json_object_s*)root->payload;
        parse_json_object(cfg, obj, NULL);
    }
    
    free(root);
    
    if (cfg->path) free(cfg->path);
    cfg->path = strdup(path);
}

sol_config* sol_config_load(const char* path) {
    sol_config* cfg = sol_config_create();
    if (cfg && path) {
        sol_config_load_file(cfg, path);
    }
    return cfg;
}

/* Load user's config from ~/.sol/config.json */
sol_config* sol_config_load_user(void) {
    sol_config* cfg = sol_config_create();
    if (!cfg) return NULL;
    
    char* path = sol_config_get_default_path();
    if (path) {
        sol_config_load_file(cfg, path);
        free(path);
    }
    
    return cfg;
}

static void config_value_destroy(sol_config_value* val) {
    if (!val) return;
    
    switch (val->type) {
        case SOL_CONFIG_STRING:
            free(val->data.string);
            break;
        case SOL_CONFIG_ARRAY:
            if (val->data.array.items) {
                for (int i = 0; i < val->data.array.count; i++) {
                    config_value_destroy(val->data.array.items[i]);
                }
                free(val->data.array.items);
            }
            break;
        case SOL_CONFIG_OBJECT:
            if (val->data.object) {
                sol_hashmap_destroy(val->data.object);
            }
            break;
        default:
            break;
    }
    
    free(val);
}

void sol_config_destroy(sol_config* cfg) {
    if (!cfg) return;
    
    if (cfg->values) {
        /* Iterate and free all values */
        sol_hashmap_iter iter;
        sol_hashmap_iter_init(&iter, cfg->values);
        const char* key;
        sol_config_value** val;
        while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&val)) {
            if (val && *val) {
                config_value_destroy(*val);
            }
        }
        sol_hashmap_destroy(cfg->values);
    }
    free(cfg->path);
    free(cfg);
}

/* Write config value as JSON */
static void write_json_value(FILE* f, sol_config_value* val) {
    if (!f || !val) return;
    
    switch (val->type) {
        case SOL_CONFIG_STRING:
            fprintf(f, "\"%s\"", val->data.string ? val->data.string : "");
            break;
        case SOL_CONFIG_INT:
            fprintf(f, "%lld", (long long)val->data.integer);
            break;
        case SOL_CONFIG_FLOAT:
            fprintf(f, "%g", val->data.number);
            break;
        case SOL_CONFIG_BOOL:
            fprintf(f, "%s", val->data.boolean ? "true" : "false");
            break;
        default:
            fprintf(f, "null");
            break;
    }
}

sol_result sol_config_save(sol_config* cfg) {
    if (!cfg || !cfg->path) return SOL_ERROR_INVALID_ARG;
    
    FILE* f = fopen(cfg->path, "w");
    if (!f) return SOL_ERROR_IO;
    
    fprintf(f, "{\n");
    
    sol_hashmap_iter iter;
    sol_hashmap_iter_init(&iter, cfg->values);
    const char* key;
    sol_config_value** val;
    bool first = true;
    
    while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&val)) {
        if (!first) fprintf(f, ",\n");
        first = false;
        
        fprintf(f, "    \"%s\": ", key);
        write_json_value(f, *val);
    }
    
    fprintf(f, "\n}\n");
    fclose(f);
    
    cfg->modified = false;
    return SOL_OK;
}

sol_config_value* sol_config_get(sol_config* cfg, const char* key) {
    if (!cfg || !key || !cfg->values) return NULL;
    
    sol_config_value** val = sol_hashmap_get_string(cfg->values, key);
    return val ? *val : NULL;
}

void sol_config_set(sol_config* cfg, const char* key, sol_config_value* value) {
    if (!cfg || !key || !cfg->values) return;
    
    /* Free existing value if any */
    sol_config_value* existing = sol_config_get(cfg, key);
    if (existing) {
        config_value_destroy(existing);
    }
    
    sol_hashmap_set_string(cfg->values, key, &value);
    cfg->modified = true;
}

void sol_config_set_string(sol_config* cfg, const char* key, const char* value) {
    sol_config_value* val = config_value_create(SOL_CONFIG_STRING);
    if (val) {
        val->data.string = strdup(value);
        sol_config_set(cfg, key, val);
    }
}

void sol_config_set_int(sol_config* cfg, const char* key, int64_t value) {
    sol_config_value* val = config_value_create(SOL_CONFIG_INT);
    if (val) {
        val->data.integer = value;
        sol_config_set(cfg, key, val);
    }
}

void sol_config_set_bool(sol_config* cfg, const char* key, bool value) {
    sol_config_value* val = config_value_create(SOL_CONFIG_BOOL);
    if (val) {
        val->data.boolean = value;
        sol_config_set(cfg, key, val);
    }
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
    return sol_theme_default_dark();
}

void sol_config_add_recent_file(sol_config* cfg, const char* path) {
    (void)cfg;
    (void)path;
    /* TODO: Add to recent files list */
}

/* Load keybindings from config file */
void sol_config_load_keybindings(sol_keymap* km, sol_config* cfg) {
    if (!km) return;
    
    char* keybind_path = sol_config_get_keybindings_path();
    if (!keybind_path) return;
    
    char* content = read_file_contents(keybind_path);
    free(keybind_path);
    if (!content) return;
    
    /* Parse JSON */
    struct json_value_s* root = json_parse(content, strlen(content));
    free(content);
    
    if (!root || root->type != json_type_object) {
        free(root);
        return;
    }
    
    struct json_object_s* obj = (struct json_object_s*)root->payload;
    struct json_object_element_s* elem = obj->start;
    
    while (elem) {
        if (strcmp(elem->name->string, "direct") == 0 && 
            elem->value->type == json_type_object) {
            /* Direct keybindings like "ctrl+s": "file.save" */
            struct json_object_s* direct = (struct json_object_s*)elem->value->payload;
            struct json_object_element_s* binding = direct->start;
            
            while (binding) {
                if (binding->value->type == json_type_string) {
                    struct json_string_s* cmd = (struct json_string_s*)binding->value->payload;
                    sol_keymap_bind(km, binding->name->string, cmd->string, NULL);
                }
                binding = binding->next;
            }
        }
        elem = elem->next;
    }
    
    free(root);
    (void)cfg;
}
