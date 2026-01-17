/*
 * Sol IDE - LSP Client
 * Language Server Protocol implementation - Stub
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* LSP client is not implemented yet - these are stubs matching the header */

sol_lsp_client* sol_lsp_client_create(const char* language_id, const char* command) {
    (void)language_id;
    (void)command;
    /* Not implemented yet */
    return NULL;
}

void sol_lsp_client_destroy(sol_lsp_client* client) {
    if (!client) return;
    /* Not implemented yet */
}

sol_result sol_lsp_start(sol_lsp_client* client, const char* root_path) {
    (void)client;
    (void)root_path;
    return SOL_ERROR_UNKNOWN;
}

void sol_lsp_stop(sol_lsp_client* client) {
    (void)client;
}

void sol_lsp_did_open(sol_lsp_client* client, sol_buffer* buf) {
    (void)client;
    (void)buf;
}

void sol_lsp_did_change(sol_lsp_client* client, sol_buffer* buf, sol_range range, const char* text) {
    (void)client;
    (void)buf;
    (void)range;
    (void)text;
}

void sol_lsp_did_save(sol_lsp_client* client, sol_buffer* buf) {
    (void)client;
    (void)buf;
}

void sol_lsp_did_close(sol_lsp_client* client, sol_buffer* buf) {
    (void)client;
    (void)buf;
}

void sol_lsp_update(sol_lsp_client* client) {
    (void)client;
}

void sol_lsp_completion(sol_lsp_client* client, sol_buffer* buf, sol_position pos,
                         sol_lsp_completion_callback callback, void* userdata) {
    (void)client;
    (void)buf;
    (void)pos;
    (void)callback;
    (void)userdata;
}

void sol_lsp_hover(sol_lsp_client* client, sol_buffer* buf, sol_position pos) {
    (void)client;
    (void)buf;
    (void)pos;
}

void sol_lsp_goto_definition(sol_lsp_client* client, sol_buffer* buf, sol_position pos) {
    (void)client;
    (void)buf;
    (void)pos;
}

void sol_lsp_find_references(sol_lsp_client* client, sol_buffer* buf, sol_position pos) {
    (void)client;
    (void)buf;
    (void)pos;
}

void sol_lsp_poll(sol_lsp_client* client) {
    (void)client;
}
