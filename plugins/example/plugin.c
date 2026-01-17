/*
 * Sol IDE - Example Plugin Template
 * 
 * Build with:
 *   gcc -shared -fPIC -o myplugin.so myplugin.c
 *   cl /LD myplugin.c /Fe:myplugin.dll (Windows)
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Plugin data */
typedef struct {
    sol_plugin_api* api;
    int counter;
} my_plugin_data;

/* Custom command */
static void cmd_hello_world(void* ctx, void* userdata) {
    my_plugin_data* data = (my_plugin_data*)userdata;
    data->counter++;
    
    char msg[64];
    snprintf(msg, sizeof(msg), "Hello from plugin! Count: %d", data->counter);
    data->api->set_status(data->api->context, msg);
}

/* Plugin initialization - called when loaded */
SOL_EXPORT bool sol_plugin_init(sol_plugin_api* api, sol_plugin_info* info) {
    /* Verify API version */
    if (api->version < SOL_PLUGIN_API_VERSION) {
        api->log(api->context, SOL_LOG_ERROR, "Plugin API version mismatch");
        return false;
    }
    
    /* Fill in plugin info */
    info->id = "example.hello";
    info->name = "Hello World Example";
    info->version = "1.0.0";
    info->description = "An example plugin demonstrating the Sol plugin API";
    info->author = "Sol Authors";
    
    return true;
}

/* Plugin activation - called when enabled */
SOL_EXPORT void* sol_plugin_activate(sol_plugin_api* api) {
    my_plugin_data* data = (my_plugin_data*)malloc(sizeof(my_plugin_data));
    if (!data) return NULL;
    
    data->api = api;
    data->counter = 0;
    
    /* Register commands */
    api->register_command(api->context, "example.hello", "Say Hello", 
                          cmd_hello_world, data);
    
    /* Bind a key */
    api->bind_key(api->context, "ctrl+shift+h", "example.hello");
    
    api->log(api->context, SOL_LOG_INFO, "Hello plugin activated!");
    
    return data;
}

/* Plugin deactivation - called when disabled or editor closes */
SOL_EXPORT void sol_plugin_deactivate(void* plugin_data) {
    my_plugin_data* data = (my_plugin_data*)plugin_data;
    if (data) {
        data->api->log(data->api->context, SOL_LOG_INFO, "Hello plugin deactivated");
        free(data);
    }
}
