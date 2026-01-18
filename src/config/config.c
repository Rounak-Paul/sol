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

/* Load keybindings config from ~/.sol/keybindings.json */
sol_config* sol_config_load_keybindings_config(void) {
    sol_config* cfg = sol_config_create();
    if (!cfg) return NULL;
    
    char* path = sol_config_get_keybindings_path();
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

typedef struct config_node {
    char* name;
    sol_config_value* value;
    sol_hashmap* children; /* name -> config_node* */
} config_node;

static config_node* config_node_create(const char* name) {
    config_node* node = calloc(1, sizeof(config_node));
    if (!node) return NULL;
    if (name) node->name = sol_strdup(name);
    node->children = sol_hashmap_create_string(sizeof(config_node*));
    return node;
}

static config_node* config_node_get_child(config_node* node, const char* name) {
    if (!node || !node->children || !name) return NULL;
    config_node** child = sol_hashmap_get_string(node->children, name);
    return child ? *child : NULL;
}

static config_node* config_node_add_child(config_node* node, const char* name) {
    if (!node || !name) return NULL;
    config_node* existing = config_node_get_child(node, name);
    if (existing) return existing;
    
    config_node* child = config_node_create(name);
    if (!child) return NULL;
    sol_hashmap_set_string(node->children, name, &child);
    return child;
}

static void config_node_destroy(config_node* node) {
    if (!node) return;
    
    if (node->children) {
        sol_hashmap_iter iter;
        sol_hashmap_iter_init(&iter, node->children);
        const char* key;
        config_node** child;
        while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&child)) {
            if (child && *child) {
                config_node_destroy(*child);
            }
        }
        sol_hashmap_destroy(node->children);
    }
    
    free(node->name);
    free(node);
}

static config_node* build_config_tree(sol_config* cfg) {
    if (!cfg || !cfg->values) return NULL;
    
    config_node* root = config_node_create(NULL);
    if (!root) return NULL;
    
    sol_hashmap_iter iter;
    sol_hashmap_iter_init(&iter, cfg->values);
    const char* key;
    sol_config_value** val;
    
    while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&val)) {
        if (!key || !val || !*val) continue;
        
        char key_buf[512];
        strncpy(key_buf, key, sizeof(key_buf) - 1);
        key_buf[sizeof(key_buf) - 1] = '\0';
        
        config_node* current = root;
        char* segment = key_buf;
        while (segment && *segment) {
            char* dot = strchr(segment, '.');
            if (dot) *dot = '\0';
            
            config_node* child = config_node_add_child(current, segment);
            if (!child) break;
            
            if (!dot) {
                child->value = *val;
                break;
            }
            
            current = child;
            segment = dot + 1;
        }
    }
    
    return root;
}

static void write_indent(FILE* f, int indent) {
    for (int i = 0; i < indent; i++) {
        fputc(' ', f);
    }
}

static void write_json_object(FILE* f, config_node* node, int indent) {
    if (!node || !node->children || sol_hashmap_count(node->children) == 0) {
        fprintf(f, "{}");
        return;
    }
    
    fprintf(f, "{\n");
    
    sol_hashmap_iter iter;
    sol_hashmap_iter_init(&iter, node->children);
    const char* key;
    config_node** child;
    bool first = true;
    
    while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&child)) {
        if (!child || !*child) continue;
        
        if (!first) fprintf(f, ",\n");
        first = false;
        
        write_indent(f, indent + 4);
        fprintf(f, "\"%s\": ", (*child)->name ? (*child)->name : "");
        
        if ((*child)->children && sol_hashmap_count((*child)->children) > 0) {
            write_json_object(f, *child, indent + 4);
        } else {
            write_json_value(f, (*child)->value);
        }
    }
    
    fprintf(f, "\n");
    write_indent(f, indent);
    fprintf(f, "}");
}

sol_result sol_config_save(sol_config* cfg) {
    if (!cfg || !cfg->path) return SOL_ERROR_INVALID_ARG;
    
    FILE* f = fopen(cfg->path, "w");
    if (!f) return SOL_ERROR_IO;
    
    config_node* root = build_config_tree(cfg);
    if (!root) {
        fclose(f);
        return SOL_ERROR_MEMORY;
    }
    
    write_json_object(f, root, 0);
    fprintf(f, "\n");
    config_node_destroy(root);
    
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

void sol_config_remove(sol_config* cfg, const char* key) {
    if (!cfg || !key || !cfg->values) return;
    
    sol_config_value** val = sol_hashmap_get_string(cfg->values, key);
    if (val && *val) {
        config_value_destroy(*val);
    }
    
    sol_hashmap_remove(cfg->values, key);
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
    if (!km || !cfg || !cfg->values) return;
    
    sol_hashmap_iter iter;
    sol_hashmap_iter_init(&iter, cfg->values);
    const char* key;
    sol_config_value** val;
    
    while (sol_hashmap_iter_next(&iter, (const void**)&key, (void**)&val)) {
        if (!key || !val || !*val) continue;
        
        if (strncmp(key, "direct.", 7) == 0 && (*val)->type == SOL_CONFIG_STRING) {
            const char* keys = key + 7;
            if (!keys || !*keys) continue;
            
            sol_keymap_unbind(km, keys);
            sol_keymap_bind(km, keys, (*val)->data.string, NULL);
        }
    }
}
