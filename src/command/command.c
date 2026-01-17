/*
 * Sol IDE - Command System
 * Registry and builtin commands
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

struct sol_command_registry {
    sol_hashmap* commands;
    sol_array(sol_command*) command_list;
};

sol_command_registry* sol_command_registry_create(void) {
    sol_command_registry* reg = (sol_command_registry*)calloc(1, sizeof(sol_command_registry));
    if (!reg) return NULL;
    
    reg->commands = sol_hashmap_create_string(sizeof(sol_command*));
    return reg;
}

void sol_command_registry_destroy(sol_command_registry* reg) {
    if (!reg) return;
    
    for (size_t i = 0; i < sol_array_count(reg->command_list); i++) {
        sol_command* cmd = reg->command_list[i];
        free((void*)cmd->id);
        free((void*)cmd->title);
        if (cmd->description) free((void*)cmd->description);
        free(cmd);
    }
    sol_array_free(reg->command_list);
    
    sol_hashmap_destroy(reg->commands);
    free(reg);
}

void sol_command_register(sol_command_registry* reg, sol_command* cmd) {
    if (!reg || !cmd || !cmd->id) return;
    
    /* Copy the command */
    sol_command* copy = (sol_command*)calloc(1, sizeof(sol_command));
    if (!copy) return;
    
    copy->id = sol_strdup(cmd->id);
    copy->title = cmd->title ? sol_strdup(cmd->title) : NULL;
    copy->description = cmd->description ? sol_strdup(cmd->description) : NULL;
    copy->handler = cmd->handler;
    copy->userdata = cmd->userdata;
    copy->enabled = true;
    
    sol_hashmap_set_string(reg->commands, copy->id, &copy);
    sol_array_push(reg->command_list, copy);
}

void sol_command_unregister(sol_command_registry* reg, const char* id) {
    if (!reg || !id) return;
    
    for (size_t i = 0; i < sol_array_count(reg->command_list); i++) {
        if (strcmp(reg->command_list[i]->id, id) == 0) {
            sol_command* cmd = reg->command_list[i];
            sol_hashmap_remove(reg->commands, id);
            sol_array_remove(reg->command_list, i);
            free((void*)cmd->id);
            free((void*)cmd->title);
            if (cmd->description) free((void*)cmd->description);
            free(cmd);
            break;
        }
    }
}

sol_command* sol_command_get(sol_command_registry* reg, const char* id) {
    if (!reg || !id) return NULL;
    
    sol_command** cmdp = (sol_command**)sol_hashmap_get_string(reg->commands, id);
    return cmdp ? *cmdp : NULL;
}

void sol_command_execute(sol_command_registry* reg, const char* id, sol_editor* ed) {
    if (!reg || !id || !ed) return;
    
    sol_command* cmd = sol_command_get(reg, id);
    if (cmd && cmd->enabled && cmd->handler) {
        cmd->handler(ed, cmd->userdata);
    }
}

sol_array(sol_command*) sol_command_list(sol_command_registry* reg) {
    if (!reg) return NULL;
    return reg->command_list;
}

sol_array(sol_command*) sol_command_search(sol_command_registry* reg, const char* query) {
    if (!reg) return NULL;
    
    sol_array(sol_command*) results = NULL;
    
    for (size_t i = 0; i < sol_array_count(reg->command_list); i++) {
        sol_command* cmd = reg->command_list[i];
        if (!query || !query[0] || 
            strstr(cmd->id, query) || 
            (cmd->title && strstr(cmd->title, query))) {
            sol_array_push(results, cmd);
        }
    }
    
    return results;
}

/* ============================================================================
 * Builtin Commands
 * ============================================================================ */

static void cmd_file_new(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_new_file(ed);
}

static void cmd_file_open(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_open_file_picker(ed);
}

static void cmd_file_open_folder(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_open_folder_picker(ed);
}

static void cmd_file_save(sol_editor* ed, void* userdata) {
    (void)userdata;
    if (ed->active_view && ed->active_view->buffer) {
        sol_buffer_save(ed->active_view->buffer);
        sol_editor_status(ed, "File saved");
    }
}

static void cmd_quit(sol_editor* ed, void* userdata) {
    (void)userdata;
    ed->running = false;
}

static void cmd_toggle_sidebar(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_toggle_sidebar(ed);
}

static void cmd_focus_sidebar(sol_editor* ed, void* userdata) {
    (void)userdata;
    if (ed->filetree) {
        ed->sidebar_visible = true;
        ed->sidebar_focused = true;
    }
}

static void cmd_toggle_terminal(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_toggle_terminal(ed);
}

static void cmd_command_palette(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_editor_open_palette(ed);
}

static void cmd_undo(sol_editor* ed, void* userdata) {
    (void)userdata;
    if (ed->active_view && ed->active_view->buffer) {
        sol_edit* edit = sol_undo(ed->active_view->buffer->undo);
        if (edit) {
            /* Apply the undo - reverse the edit */
            sol_buffer* buf = ed->active_view->buffer;
            if (edit->type == SOL_EDIT_INSERT) {
                /* Undo insert = delete */
                sol_position start = sol_piece_table_offset_to_pos(buf->text, edit->offset);
                sol_position end = sol_piece_table_offset_to_pos(buf->text, edit->offset + edit->new_len);
                sol_range r = {start, end};
                sol_piece_table_delete(buf->text, edit->offset, edit->new_len);
                ed->active_view->cursor.pos = start;
            } else {
                /* Undo delete = insert */
                sol_piece_table_insert(buf->text, edit->offset, edit->old_text, edit->old_len);
                ed->active_view->cursor.pos = sol_piece_table_offset_to_pos(buf->text, edit->offset + edit->old_len);
            }
            sol_editor_status(ed, "Undo");
        } else {
            sol_editor_status(ed, "Nothing to undo");
        }
    }
}

static void cmd_redo(sol_editor* ed, void* userdata) {
    (void)userdata;
    if (ed->active_view && ed->active_view->buffer) {
        sol_edit* edit = sol_redo(ed->active_view->buffer->undo);
        if (edit) {
            /* Apply the redo - apply the edit */
            sol_buffer* buf = ed->active_view->buffer;
            if (edit->type == SOL_EDIT_INSERT) {
                /* Redo insert */
                sol_piece_table_insert(buf->text, edit->offset, edit->new_text, edit->new_len);
                ed->active_view->cursor.pos = sol_piece_table_offset_to_pos(buf->text, edit->offset + edit->new_len);
            } else {
                /* Redo delete */
                sol_piece_table_delete(buf->text, edit->offset, edit->old_len);
                sol_position pos = sol_piece_table_offset_to_pos(buf->text, edit->offset);
                ed->active_view->cursor.pos = pos;
            }
            sol_editor_status(ed, "Redo");
        } else {
            sol_editor_status(ed, "Nothing to redo");
        }
    }
}

static void cmd_close_view(sol_editor* ed, void* userdata) {
    (void)userdata;
    if (ed->active_view) {
        sol_editor_close_view(ed, ed->active_view);
    }
}

static void cmd_keybindings(sol_editor* ed, void* userdata) {
    (void)userdata;
    sol_keybindings_open(ed);
}

void sol_commands_register_builtin(sol_command_registry* reg) {
    if (!reg) return;
    
    sol_command cmds[] = {
        { "file.new", "New File", "Create a new file", cmd_file_new, NULL, true },
        { "file.open", "Open File", "Open an existing file", cmd_file_open, NULL, true },
        { "file.openFolder", "Open Folder", "Open a folder as workspace", cmd_file_open_folder, NULL, true },
        { "file.save", "Save", "Save the current file", cmd_file_save, NULL, true },
        { "app.quit", "Quit", "Exit Sol", cmd_quit, NULL, true },
        { "view.toggleSidebar", "Toggle Sidebar", "Show/hide sidebar", cmd_toggle_sidebar, NULL, true },
        { "view.focusSidebar", "Focus Sidebar", "Focus the file explorer", cmd_focus_sidebar, NULL, true },
        { "view.toggleTerminal", "Toggle Terminal", "Show/hide terminal", cmd_toggle_terminal, NULL, true },
        { "view.commandPalette", "Command Palette", "Open command palette", cmd_command_palette, NULL, true },
        { "view.keybindings", "Keybindings", "Edit keybindings", cmd_keybindings, NULL, true },
        { "view.close", "Close", "Close current view", cmd_close_view, NULL, true },
        { "edit.undo", "Undo", "Undo last edit", cmd_undo, NULL, true },
        { "edit.redo", "Redo", "Redo last undone edit", cmd_redo, NULL, true },
    };
    
    size_t count = sizeof(cmds) / sizeof(cmds[0]);
    for (size_t i = 0; i < count; i++) {
        sol_command_register(reg, &cmds[i]);
    }
}
