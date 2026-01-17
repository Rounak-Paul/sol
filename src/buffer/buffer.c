/*
 * Sol IDE - Buffer Implementation
 * 
 * A buffer represents a text file in memory.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint32_t next_buffer_id = 1;

/* Detect language from file extension */
static const char* detect_language(const char* filepath) {
    if (!filepath) return "text";
    
    const char* ext = sol_path_extension(filepath);
    if (!ext) return "text";
    
    /* Common extensions */
    if (strcmp(ext, "c") == 0) return "c";
    if (strcmp(ext, "h") == 0) return "c";
    if (strcmp(ext, "cpp") == 0 || strcmp(ext, "cc") == 0 || 
        strcmp(ext, "cxx") == 0) return "cpp";
    if (strcmp(ext, "hpp") == 0 || strcmp(ext, "hh") == 0) return "cpp";
    if (strcmp(ext, "py") == 0) return "python";
    if (strcmp(ext, "js") == 0) return "javascript";
    if (strcmp(ext, "ts") == 0) return "typescript";
    if (strcmp(ext, "jsx") == 0) return "javascriptreact";
    if (strcmp(ext, "tsx") == 0) return "typescriptreact";
    if (strcmp(ext, "rs") == 0) return "rust";
    if (strcmp(ext, "go") == 0) return "go";
    if (strcmp(ext, "java") == 0) return "java";
    if (strcmp(ext, "rb") == 0) return "ruby";
    if (strcmp(ext, "lua") == 0) return "lua";
    if (strcmp(ext, "sh") == 0 || strcmp(ext, "bash") == 0) return "shellscript";
    if (strcmp(ext, "json") == 0) return "json";
    if (strcmp(ext, "yaml") == 0 || strcmp(ext, "yml") == 0) return "yaml";
    if (strcmp(ext, "toml") == 0) return "toml";
    if (strcmp(ext, "xml") == 0) return "xml";
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "html";
    if (strcmp(ext, "css") == 0) return "css";
    if (strcmp(ext, "scss") == 0) return "scss";
    if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0) return "markdown";
    if (strcmp(ext, "sql") == 0) return "sql";
    if (strcmp(ext, "zig") == 0) return "zig";
    if (strcmp(ext, "odin") == 0) return "odin";
    
    return "text";
}

/* Detect line ending style */
static sol_line_ending detect_line_ending(const char* content, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (content[i] == '\r') {
            if (i + 1 < len && content[i + 1] == '\n') {
                return SOL_LINE_ENDING_CRLF;
            }
            return SOL_LINE_ENDING_CR;
        }
        if (content[i] == '\n') {
            return SOL_LINE_ENDING_LF;
        }
    }
    
    /* Default based on platform */
#ifdef SOL_PLATFORM_WINDOWS
    return SOL_LINE_ENDING_CRLF;
#else
    return SOL_LINE_ENDING_LF;
#endif
}

sol_buffer* sol_buffer_create(const char* name) {
    sol_buffer* buf = (sol_buffer*)calloc(1, sizeof(sol_buffer));
    if (!buf) return NULL;
    
    buf->id = next_buffer_id++;
    buf->name = sol_strdup(name ? name : "Untitled");
    buf->type = SOL_BUFFER_SCRATCH;
    buf->encoding = SOL_ENCODING_UTF8;
    buf->line_ending = SOL_LINE_ENDING_LF;
    
    buf->text = sol_piece_table_create(NULL, 0);
    if (!buf->text) {
        free(buf->name);
        free(buf);
        return NULL;
    }
    
    buf->undo = sol_undo_tree_create();
    if (!buf->undo) {
        sol_piece_table_destroy(buf->text);
        free(buf->name);
        free(buf);
        return NULL;
    }
    
    buf->language = "text";
    
    return buf;
}

sol_buffer* sol_buffer_open(const char* filepath) {
    if (!filepath) return NULL;
    
    size_t len;
    char* content = sol_file_read(filepath, &len);
    if (!content) {
        /* File doesn't exist - create new buffer with filepath */
        sol_buffer* buf = sol_buffer_create(sol_path_basename(filepath));
        if (buf) {
            buf->filepath = sol_strdup(filepath);
            buf->type = SOL_BUFFER_NORMAL;
            buf->language = detect_language(filepath);
        }
        return buf;
    }
    
    sol_buffer* buf = (sol_buffer*)calloc(1, sizeof(sol_buffer));
    if (!buf) {
        free(content);
        return NULL;
    }
    
    buf->id = next_buffer_id++;
    buf->filepath = sol_strdup(filepath);
    buf->name = sol_strdup(sol_path_basename(filepath));
    buf->type = SOL_BUFFER_NORMAL;
    buf->encoding = SOL_ENCODING_UTF8;
    buf->line_ending = detect_line_ending(content, len);
    buf->file_mtime = sol_file_mtime(filepath);
    buf->language = detect_language(filepath);
    
    /* Normalize line endings to LF internally */
    /* TODO: Proper normalization */
    
    buf->text = sol_piece_table_create(content, len);
    free(content);
    
    if (!buf->text) {
        free(buf->filepath);
        free(buf->name);
        free(buf);
        return NULL;
    }
    
    buf->undo = sol_undo_tree_create();
    if (!buf->undo) {
        sol_piece_table_destroy(buf->text);
        free(buf->filepath);
        free(buf->name);
        free(buf);
        return NULL;
    }
    
    return buf;
}

void sol_buffer_destroy(sol_buffer* buf) {
    if (!buf) return;
    
    sol_piece_table_destroy(buf->text);
    sol_undo_tree_destroy(buf->undo);
    free(buf->filepath);
    free(buf->name);
    /* marks and search_matches arrays */
    sol_array_free(buf->marks);
    sol_array_free(buf->search_matches);
    free(buf);
}

sol_result sol_buffer_save(sol_buffer* buf) {
    if (!buf) return SOL_ERROR_INVALID_ARG;
    if (!buf->filepath) return SOL_ERROR_INVALID_ARG;
    if (buf->readonly) return SOL_ERROR_PERMISSION;
    
    /* Get all text */
    size_t len = sol_piece_table_length(buf->text);
    char* content = (char*)malloc(len + 1);
    if (!content) return SOL_ERROR_MEMORY;
    
    sol_piece_table_get_text(buf->text, 0, len, content);
    content[len] = '\0';
    
    /* TODO: Convert line endings if needed */
    
    sol_result result = sol_file_write(buf->filepath, content, len);
    free(content);
    
    if (result == SOL_OK) {
        buf->modified = false;
        buf->file_mtime = sol_file_mtime(buf->filepath);
    }
    
    return result;
}

sol_result sol_buffer_save_as(sol_buffer* buf, const char* filepath) {
    if (!buf || !filepath) return SOL_ERROR_INVALID_ARG;
    
    char* old_path = buf->filepath;
    buf->filepath = sol_strdup(filepath);
    
    if (buf->name) free(buf->name);
    buf->name = sol_strdup(sol_path_basename(filepath));
    
    sol_result result = sol_buffer_save(buf);
    
    if (result != SOL_OK) {
        /* Restore old path on failure */
        free(buf->filepath);
        buf->filepath = old_path;
    } else {
        free(old_path);
        buf->language = detect_language(filepath);
    }
    
    return result;
}

sol_result sol_buffer_reload(sol_buffer* buf) {
    if (!buf || !buf->filepath) return SOL_ERROR_INVALID_ARG;
    
    size_t len;
    char* content = sol_file_read(buf->filepath, &len);
    if (!content) return SOL_ERROR_IO;
    
    /* Replace piece table */
    sol_piece_table_destroy(buf->text);
    buf->text = sol_piece_table_create(content, len);
    free(content);
    
    if (!buf->text) return SOL_ERROR_MEMORY;
    
    /* Reset undo history */
    sol_undo_tree_destroy(buf->undo);
    buf->undo = sol_undo_tree_create();
    
    buf->modified = false;
    buf->file_mtime = sol_file_mtime(buf->filepath);
    
    return SOL_OK;
}

void sol_buffer_insert(sol_buffer* buf, sol_position pos, const char* text, size_t len) {
    if (!buf || !text || len == 0) return;
    
    size_t offset = sol_piece_table_pos_to_offset(buf->text, pos);
    
    /* Record for undo */
    sol_edit edit = {0};
    edit.type = SOL_EDIT_INSERT;
    edit.offset = offset;
    edit.new_len = len;
    edit.new_text = sol_strndup(text, len);
    edit.cursor_before = pos;
    edit.timestamp = sol_time_ms();
    
    /* Perform insert */
    sol_piece_table_insert(buf->text, offset, text, len);
    
    /* Calculate cursor after */
    edit.cursor_after = sol_piece_table_offset_to_pos(buf->text, offset + len);
    
    sol_undo_record(buf->undo, &edit);
    
    buf->modified = true;
    buf->lsp_version++;
}

void sol_buffer_delete(sol_buffer* buf, sol_range range) {
    if (!buf) return;
    
    size_t start = sol_piece_table_pos_to_offset(buf->text, range.start);
    size_t end = sol_piece_table_pos_to_offset(buf->text, range.end);
    
    if (start >= end) return;
    
    size_t len = end - start;
    
    /* Record for undo */
    sol_edit edit = {0};
    edit.type = SOL_EDIT_DELETE;
    edit.offset = start;
    edit.old_len = len;
    edit.old_text = (char*)malloc(len + 1);
    if (edit.old_text) {
        sol_piece_table_get_text(buf->text, start, len, edit.old_text);
        edit.old_text[len] = '\0';
    }
    edit.cursor_before = range.end;
    edit.timestamp = sol_time_ms();
    
    /* Perform delete */
    sol_piece_table_delete(buf->text, start, len);
    
    edit.cursor_after = range.start;
    
    sol_undo_record(buf->undo, &edit);
    
    buf->modified = true;
    buf->lsp_version++;
}

void sol_buffer_replace(sol_buffer* buf, sol_range range, const char* text, size_t len) {
    if (!buf) return;
    
    sol_undo_begin_group(buf->undo);
    sol_buffer_delete(buf, range);
    sol_buffer_insert(buf, range.start, text, len);
    sol_undo_end_group(buf->undo);
}

size_t sol_buffer_get_line(sol_buffer* buf, int32_t line, char* out, size_t out_size) {
    if (!buf) return 0;
    return sol_piece_table_get_line(buf->text, line, out, out_size);
}

int32_t sol_buffer_line_count(sol_buffer* buf) {
    if (!buf) return 0;
    return sol_piece_table_line_count(buf->text);
}

size_t sol_buffer_length(sol_buffer* buf) {
    if (!buf) return 0;
    return sol_piece_table_length(buf->text);
}

bool sol_buffer_is_modified(sol_buffer* buf) {
    return buf ? buf->modified : false;
}

const char* sol_buffer_get_language(sol_buffer* buf) {
    return buf ? buf->language : "text";
}

int32_t sol_buffer_line_length(sol_buffer* buf, int32_t line) {
    if (!buf) return 0;
    return (int32_t)sol_piece_table_line_length(buf->text, line);
}
