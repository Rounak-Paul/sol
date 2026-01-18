/*
 * flow.c - Vim-like command flow system implementation
 */

#include "flow.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Flow command definition */
typedef struct {
    char key;               /* Command key (lowercase) */
    bool shift;             /* Requires shift (uppercase) */
    bool supports_count;    /* Supports count prefix */
    const char* command;    /* Command name to execute */
    const char* description;/* Human readable description */
} sol_flow_command;

/* Flow command table */
static const sol_flow_command s_flow_commands[] = {
    /* File operations */
    {'o', false, false, "file.open",        "Open file"},
    {'o', true,  false, "file.openFolder",  "Open folder"},
    {'n', false, false, "file.new",         "New file"},
    {'s', false, false, "file.save",        "Save file"},
    {'s', true,  false, "file.saveAll",     "Save all files"},
    {'w', false, false, "view.close",       "Close buffer"},
    {'w', true,  false, "buffer.closeAll",  "Close all buffers"},
    
    /* Edit operations */
    {'x', false, true,  "edit.undo",        "Undo"},
    {'x', true,  false, "edit.undoAll",     "Undo all"},
    {'r', false, true,  "edit.redo",        "Redo"},
    {'r', true,  false, "edit.redoAll",     "Redo all"},
    {'c', false, false, "edit.copy",        "Copy"},
    {'v', false, false, "edit.paste",       "Paste"},
    {'d', false, false, "edit.cut",         "Cut"},
    
    /* Navigation */
    {'f', false, false, "find.open",        "Find"},
    {'/', false, false, "find.open",        "Search"},
    {'g', false, false, "goto.line",        "Go to line"},
    {'e', false, false, "view.focusSidebar","Focus explorer"},
    
    /* View/UI */
    {'p', false, false, "view.commandPalette", "Command palette"},
    {'b', false, false, "view.toggleSidebar",  "Toggle sidebar"},
    {'t', false, false, "view.toggleTerminal", "Toggle terminal"},
    {'k', false, false, "view.keybindings",    "Edit keybindings"},
    
    /* App control */
    {'q', false, false, "app.quit",         "Quit"},
    
    /* Sentinel */
    {0, false, false, NULL, NULL}
};

static bool flow_command_supports_count(const char* command) {
    if (!command) return false;
    return strcmp(command, "edit.undo") == 0 || strcmp(command, "edit.redo") == 0;
}

static const char* flow_command_description(const char* command) {
    if (!command) return "";
    for (int i = 0; s_flow_commands[i].command != NULL; i++) {
        if (strcmp(s_flow_commands[i].command, command) == 0) {
            return s_flow_commands[i].description;
        }
    }
    return command;
}

/* Timeout for flow mode (ms) */
#define FLOW_TIMEOUT_MS 3000

bool sol_flow_is_leader(sol_keychord* chord) {
    if (!chord) return false;
    /* Leader is Ctrl+Space (key is space with ctrl modifier) */
    return (chord->mods & SOL_MOD_CTRL) && (chord->key == ' ' || chord->key == 0);
}

/* Find command in flow table */
static const sol_flow_command* find_flow_command(sol_editor* ed, char key, bool shift) {
    char normalized = key;
    if (normalized >= 'a' && normalized <= 'z') {
        normalized = shift ? (char)(normalized - 32) : normalized;
    } else if (normalized >= 'A' && normalized <= 'Z') {
        normalized = shift ? normalized : (char)(normalized + 32);
    }
    
    if (ed && ed->keybind_config) {
        char lookup_key[32];
        snprintf(lookup_key, sizeof(lookup_key), "flow.commands.%c", normalized);
        const char* cmd = sol_config_get_string(ed->keybind_config, lookup_key, NULL);
        if (cmd) {
            static sol_flow_command override;
            override.key = (char)tolower((unsigned char)normalized);
            override.shift = shift;
            override.supports_count = flow_command_supports_count(cmd);
            override.command = cmd;
            override.description = flow_command_description(cmd);
            return &override;
        }
    }
    
    char lower_key = (normalized >= 'A' && normalized <= 'Z') ? normalized + 32 : normalized;
    for (int i = 0; s_flow_commands[i].command != NULL; i++) {
        if (s_flow_commands[i].key == lower_key && 
            s_flow_commands[i].shift == shift) {
            return &s_flow_commands[i];
        }
    }
    return NULL;
}

/* Execute flow command with count */
static void execute_flow_command(sol_editor* ed, const sol_flow_command* cmd, int count) {
    if (!ed || !cmd) return;
    
    /* Execute command count times (or once if count is 0) */
    int times = (count > 0) ? count : 1;
    
    for (int i = 0; i < times; i++) {
        sol_command_execute(ed->commands, cmd->command, ed);
    }
    
    /* Show status message */
    if (count > 1 && cmd->supports_count) {
        sol_editor_status(ed, "%s (x%d)", cmd->description, times);
    } else {
        sol_editor_status(ed, "%s", cmd->description);
    }
}

bool sol_flow_handle_key(sol_editor* ed, sol_keychord* chord) {
    if (!ed || !chord) return false;
    
    /* If not in flow mode, check for leader key */
    if (!ed->flow_active) {
        if (sol_flow_is_leader(chord)) {
            /* Activate flow mode */
            ed->flow_active = true;
            ed->flow_count = 0;
            ed->flow_key_count = 0;
            memset(ed->flow_keys, 0, sizeof(ed->flow_keys));
            ed->flow_start_time = sol_time_ms();
            sol_editor_status(ed, "-- COMMAND --");
            return true;
        }
        return false;
    }
    
    /* Check for timeout */
    if (sol_time_ms() - ed->flow_start_time > FLOW_TIMEOUT_MS) {
        sol_flow_cancel(ed);
        sol_editor_status(ed, "Command timeout");
        return true;
    }
    
    /* Escape cancels flow mode */
    if (chord->key == SOL_KEY_ESCAPE) {
        sol_flow_cancel(ed);
        sol_editor_status(ed, "");
        return true;
    }
    
    /* Ignore modifier-only keys */
    if (chord->mods & (SOL_MOD_CTRL | SOL_MOD_ALT)) {
        /* If they press Ctrl+Space again, just reset */
        if (sol_flow_is_leader(chord)) {
            ed->flow_count = 0;
            ed->flow_key_count = 0;
            memset(ed->flow_keys, 0, sizeof(ed->flow_keys));
            sol_editor_status(ed, "-- COMMAND --");
        }
        return true;
    }
    
    /* Check if it's a digit (count prefix) */
    char key = (char)chord->key;
    if (key >= '0' && key <= '9') {
        /* Don't allow leading zero for count */
        if (key == '0' && ed->flow_count == 0) {
            /* '0' could be a command in the future, for now ignore */
            return true;
        }
        ed->flow_count = ed->flow_count * 10 + (key - '0');
        /* Cap count at reasonable value */
        if (ed->flow_count > 9999) ed->flow_count = 9999;
        
        /* Update pending keys display */
        if (ed->flow_key_count < (int)sizeof(ed->flow_keys) - 1) {
            ed->flow_keys[ed->flow_key_count++] = key;
        }
        
        /* Update status to show pending count */
        char status[64];
        sol_flow_status_string(ed, status, sizeof(status));
        sol_editor_status(ed, "%s", status);
        return true;
    }
    
    /* Check for shift (uppercase) */
    bool shift = (chord->mods & SOL_MOD_SHIFT) != 0;
    
    /* Look up command */
    const sol_flow_command* cmd = find_flow_command(ed, key, shift);
    
    if (cmd) {
        /* Execute the command */
        execute_flow_command(ed, cmd, ed->flow_count);
        
        /* Exit flow mode after executing */
        sol_flow_cancel(ed);
        return true;
    }
    
    /* Unknown key - show error and stay in flow mode */
    char lower_key = (key >= 'A' && key <= 'Z') ? key + 32 : key;
    sol_editor_status(ed, "Unknown command: %c%s", 
                      shift ? toupper(lower_key) : lower_key,
                      shift ? " (shift)" : "");
    return true;
}

void sol_flow_cancel(sol_editor* ed) {
    if (!ed) return;
    ed->flow_active = false;
    ed->flow_count = 0;
    ed->flow_key_count = 0;
    memset(ed->flow_keys, 0, sizeof(ed->flow_keys));
}

void sol_flow_status_string(sol_editor* ed, char* buf, size_t len) {
    if (!ed || !buf || len == 0) return;
    
    if (!ed->flow_active) {
        buf[0] = '\0';
        return;
    }
    
    if (ed->flow_count > 0) {
        snprintf(buf, len, "-- COMMAND %d --", ed->flow_count);
    } else {
        snprintf(buf, len, "-- COMMAND --");
    }
}
