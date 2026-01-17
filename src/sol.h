/*
 * Sol - A Modern Terminal-Based IDE
 * 
 * Copyright (c) 2026
 * Licensed under MIT License
 * 
 * Sol is a hyper-extensible, modeless terminal IDE with:
 * - LSP support for intelligent code completion
 * - Tree-sitter based syntax highlighting
 * - Built-in terminal emulator
 * - Git integration
 * - Plugin system via shared libraries
 * - Vim-like logical key sequences in a modeless environment
 */

#ifndef SOL_H
#define SOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations for tuih types - include tui.h before sol.h in implementation files */
#ifndef TUI_H
struct tui_context;
typedef struct tui_context tui_context;
struct tui_event_struct;
typedef struct tui_event_struct tui_event;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version and Build Info
 * ============================================================================ */

#define SOL_VERSION_MAJOR 0
#define SOL_VERSION_MINOR 1
#define SOL_VERSION_PATCH 0
#define SOL_VERSION_STRING "0.1.0"

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

#if defined(_WIN32) || defined(_WIN64)
    #define SOL_PLATFORM_WINDOWS 1
    #define SOL_EXPORT __declspec(dllexport)
    #define SOL_IMPORT __declspec(dllimport)
    #define SOL_PATH_SEPARATOR '\\'
    #define SOL_PATH_SEPARATOR_STR "\\"
#else
    #define SOL_PLATFORM_POSIX 1
    #define SOL_EXPORT __attribute__((visibility("default")))
    #define SOL_IMPORT
    #define SOL_PATH_SEPARATOR '/'
    #define SOL_PATH_SEPARATOR_STR "/"
#endif

#if defined(__APPLE__)
    #define SOL_PLATFORM_MACOS 1
#elif defined(__linux__)
    #define SOL_PLATFORM_LINUX 1
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #define SOL_PLATFORM_BSD 1
#endif

#ifdef SOL_BUILD_DLL
    #define SOL_API SOL_EXPORT
#else
    #define SOL_API SOL_IMPORT
#endif

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

/* Core types */
typedef struct sol_editor sol_editor;
typedef struct sol_buffer sol_buffer;
typedef struct sol_view sol_view;
typedef struct sol_window sol_window;
typedef struct sol_panel sol_panel;
typedef struct sol_command sol_command;
typedef struct sol_keybind sol_keybind;
typedef struct sol_plugin sol_plugin;
typedef struct sol_config sol_config;
typedef struct sol_theme sol_theme;

/* Subsystems */
typedef struct sol_lsp_client sol_lsp_client;
typedef struct sol_terminal sol_terminal;
typedef struct sol_git sol_git;
typedef struct sol_filetree sol_filetree;

/* ============================================================================
 * Result/Error Handling
 * ============================================================================ */

typedef enum {
    SOL_OK = 0,
    SOL_ERROR_MEMORY,
    SOL_ERROR_IO,
    SOL_ERROR_NOT_FOUND,
    SOL_ERROR_INVALID_ARG,
    SOL_ERROR_PERMISSION,
    SOL_ERROR_ALREADY_EXISTS,
    SOL_ERROR_BUSY,
    SOL_ERROR_TIMEOUT,
    SOL_ERROR_CANCELLED,
    SOL_ERROR_PLUGIN,
    SOL_ERROR_PARSE,
    SOL_ERROR_ENCODING,
    SOL_ERROR_UNKNOWN
} sol_result;

/* Get human-readable error message */
SOL_API const char* sol_result_string(sol_result result);

/* ============================================================================
 * Memory Management - Arena Allocator
 * ============================================================================ */

#define SOL_ARENA_BLOCK_SIZE (64 * 1024)  /* 64KB blocks */

typedef struct sol_arena_block {
    struct sol_arena_block* next;
    size_t size;
    size_t used;
    uint8_t data[];
} sol_arena_block;

typedef struct sol_arena {
    sol_arena_block* first;
    sol_arena_block* current;
    size_t total_allocated;
    size_t total_used;
} sol_arena;

SOL_API sol_arena* sol_arena_create(void);
SOL_API void sol_arena_destroy(sol_arena* arena);
SOL_API void* sol_arena_alloc(sol_arena* arena, size_t size);
SOL_API void* sol_arena_alloc_zero(sol_arena* arena, size_t size);
SOL_API char* sol_arena_strdup(sol_arena* arena, const char* str);
SOL_API char* sol_arena_strndup(sol_arena* arena, const char* str, size_t len);
SOL_API void sol_arena_reset(sol_arena* arena);

/* ============================================================================
 * String Interning (for repeated strings like filenames)
 * ============================================================================ */

typedef struct sol_intern sol_intern;

SOL_API sol_intern* sol_intern_create(void);
SOL_API void sol_intern_destroy(sol_intern* intern);
SOL_API const char* sol_intern_string(sol_intern* intern, const char* str);
SOL_API const char* sol_intern_stringn(sol_intern* intern, const char* str, size_t len);

/* ============================================================================
 * Dynamic Array (type-safe via macros)
 * ============================================================================ */

#define SOL_ARRAY_INITIAL_CAP 16

typedef struct {
    void* data;
    size_t count;
    size_t capacity;
    size_t elem_size;
} sol_array_header;

SOL_API void* sol_array_grow(void* arr, size_t elem_size, size_t min_cap);
SOL_API void sol_array_free(void* arr);

#define sol_array(T) T*

#define sol_array_header(arr) \
    ((sol_array_header*)((char*)(arr) - sizeof(sol_array_header)))

#define sol_array_count(arr) \
    ((arr) ? sol_array_header(arr)->count : 0)

#define sol_array_capacity(arr) \
    ((arr) ? sol_array_header(arr)->capacity : 0)

#define sol_array_push(arr, val) do { \
    if (!(arr) || sol_array_count(arr) >= sol_array_capacity(arr)) { \
        (arr) = sol_array_grow((arr), sizeof(*(arr)), sol_array_count(arr) + 1); \
    } \
    (arr)[sol_array_header(arr)->count++] = (val); \
} while(0)

#define sol_array_pop(arr) \
    ((arr) && sol_array_count(arr) > 0 ? (arr)[--sol_array_header(arr)->count] : (arr)[0])

#define sol_array_last(arr) \
    ((arr)[sol_array_count(arr) - 1])

#define sol_array_clear(arr) \
    do { if (arr) sol_array_header(arr)->count = 0; } while(0)

#define sol_array_remove(arr, idx) do { \
    if ((arr) && (idx) < sol_array_count(arr)) { \
        size_t _i = (idx); \
        size_t _count = sol_array_count(arr); \
        for (size_t _j = _i; _j < _count - 1; _j++) { \
            (arr)[_j] = (arr)[_j + 1]; \
        } \
        sol_array_header(arr)->count--; \
    } \
} while(0)

#define sol_array_free(arr) \
    do { if (arr) { free(sol_array_header(arr)); (arr) = NULL; } } while(0)

/* ============================================================================
 * Hash Map
 * ============================================================================ */

typedef struct sol_hashmap sol_hashmap;
typedef uint64_t (*sol_hash_fn)(const void* key, size_t len);
typedef bool (*sol_equal_fn)(const void* a, const void* b, size_t len);

SOL_API sol_hashmap* sol_hashmap_create(size_t key_size, size_t value_size);
SOL_API sol_hashmap* sol_hashmap_create_string(size_t value_size);
SOL_API void sol_hashmap_destroy(sol_hashmap* map);
SOL_API bool sol_hashmap_set(sol_hashmap* map, const void* key, const void* value);
SOL_API void* sol_hashmap_get(sol_hashmap* map, const void* key);
SOL_API bool sol_hashmap_remove(sol_hashmap* map, const void* key);
SOL_API size_t sol_hashmap_count(sol_hashmap* map);
SOL_API void sol_hashmap_clear(sol_hashmap* map);

/* String-keyed convenience functions */
SOL_API bool sol_hashmap_set_string(sol_hashmap* map, const char* key, const void* value);
SOL_API void* sol_hashmap_get_string(sol_hashmap* map, const char* key);

/* ============================================================================
 * Text Position and Range
 * ============================================================================ */

typedef struct {
    int32_t line;       /* 0-indexed line number */
    int32_t column;     /* 0-indexed column (byte offset in line) */
} sol_position;

typedef struct {
    sol_position start;
    sol_position end;
} sol_range;

typedef struct {
    size_t offset;      /* Byte offset from start of buffer */
} sol_offset;

/* ============================================================================
 * Piece Table - Core Text Data Structure
 * ============================================================================ */

typedef enum {
    SOL_PIECE_ORIGINAL,     /* Points to original file content */
    SOL_PIECE_ADD           /* Points to add buffer (new text) */
} sol_piece_source;

typedef struct sol_piece {
    struct sol_piece* prev;
    struct sol_piece* next;
    sol_piece_source source;
    size_t start;           /* Start offset in source buffer */
    size_t length;          /* Length in bytes */
    int32_t line_count;     /* Cached: number of newlines in this piece */
} sol_piece;

typedef struct {
    char* original;         /* Original file content (immutable) */
    size_t original_len;
    char* add;              /* Add buffer (append-only) */
    size_t add_len;
    size_t add_cap;
    sol_piece* first;       /* First piece in list */
    sol_piece* last;        /* Last piece in list */
    sol_arena* piece_arena; /* Arena for piece allocations */
    size_t total_length;    /* Total text length */
    int32_t total_lines;    /* Total line count */
} sol_piece_table;

SOL_API sol_piece_table* sol_piece_table_create(const char* initial, size_t len);
SOL_API void sol_piece_table_destroy(sol_piece_table* pt);
SOL_API sol_result sol_piece_table_insert(sol_piece_table* pt, size_t offset, const char* text, size_t len);
SOL_API sol_result sol_piece_table_delete(sol_piece_table* pt, size_t offset, size_t len);
SOL_API size_t sol_piece_table_length(sol_piece_table* pt);
SOL_API int32_t sol_piece_table_line_count(sol_piece_table* pt);
SOL_API char sol_piece_table_char_at(sol_piece_table* pt, size_t offset);
SOL_API size_t sol_piece_table_get_text(sol_piece_table* pt, size_t offset, size_t len, char* out);
SOL_API size_t sol_piece_table_get_line(sol_piece_table* pt, int32_t line, char* out, size_t out_size);
SOL_API size_t sol_piece_table_line_start(sol_piece_table* pt, int32_t line);
SOL_API size_t sol_piece_table_line_length(sol_piece_table* pt, int32_t line);
SOL_API sol_position sol_piece_table_offset_to_pos(sol_piece_table* pt, size_t offset);
SOL_API size_t sol_piece_table_pos_to_offset(sol_piece_table* pt, sol_position pos);

/* ============================================================================
 * Undo/Redo System - Tree-based
 * ============================================================================ */

typedef enum {
    SOL_EDIT_INSERT,
    SOL_EDIT_DELETE,
    SOL_EDIT_REPLACE,       /* Convenience: delete + insert */
    SOL_EDIT_GROUP_START,   /* Start of grouped operations */
    SOL_EDIT_GROUP_END      /* End of grouped operations */
} sol_edit_type;

typedef struct sol_edit {
    sol_edit_type type;
    size_t offset;
    size_t old_len;         /* For delete/replace: length of removed text */
    size_t new_len;         /* For insert/replace: length of inserted text */
    char* old_text;         /* For delete/replace: removed text */
    char* new_text;         /* For insert/replace: inserted text */
    sol_position cursor_before;
    sol_position cursor_after;
    uint64_t timestamp;
} sol_edit;

typedef struct sol_undo_node {
    struct sol_undo_node* parent;
    struct sol_undo_node** children;
    int child_count;
    int child_capacity;
    int active_child;       /* Index of child that's in current timeline */
    sol_edit edit;
    uint32_t id;            /* Unique node ID for tree visualization */
} sol_undo_node;

typedef struct {
    sol_undo_node* root;
    sol_undo_node* current;
    sol_arena* arena;
    uint32_t next_id;
    int group_depth;        /* For nested undo groups */
} sol_undo_tree;

SOL_API sol_undo_tree* sol_undo_tree_create(void);
SOL_API void sol_undo_tree_destroy(sol_undo_tree* tree);
SOL_API void sol_undo_record(sol_undo_tree* tree, sol_edit* edit);
SOL_API sol_edit* sol_undo(sol_undo_tree* tree);
SOL_API sol_edit* sol_redo(sol_undo_tree* tree);
SOL_API void sol_undo_begin_group(sol_undo_tree* tree);
SOL_API void sol_undo_end_group(sol_undo_tree* tree);
SOL_API bool sol_undo_can_undo(sol_undo_tree* tree);
SOL_API bool sol_undo_can_redo(sol_undo_tree* tree);
SOL_API int sol_undo_branch_count(sol_undo_tree* tree);
SOL_API void sol_undo_switch_branch(sol_undo_tree* tree, int branch_index);

/* ============================================================================
 * Buffer - A text file in memory
 * ============================================================================ */

typedef enum {
    SOL_BUFFER_NORMAL,
    SOL_BUFFER_READONLY,
    SOL_BUFFER_SCRATCH,     /* Unsaved, no file */
    SOL_BUFFER_TERMINAL     /* Terminal buffer */
} sol_buffer_type;

typedef enum {
    SOL_ENCODING_UTF8,
    SOL_ENCODING_UTF16_LE,
    SOL_ENCODING_UTF16_BE,
    SOL_ENCODING_LATIN1
} sol_encoding;

typedef enum {
    SOL_LINE_ENDING_LF,
    SOL_LINE_ENDING_CRLF,
    SOL_LINE_ENDING_CR
} sol_line_ending;

struct sol_buffer {
    uint32_t id;
    char* filepath;             /* NULL for scratch buffers */
    char* name;                 /* Display name */
    sol_buffer_type type;
    sol_encoding encoding;
    sol_line_ending line_ending;
    sol_piece_table* text;
    sol_undo_tree* undo;
    bool modified;
    bool readonly;
    uint64_t file_mtime;        /* Last modification time */
    /* Syntax highlighting */
    void* syntax_tree;          /* Tree-sitter tree */
    const char* language;       /* Detected language ID */
    /* LSP state */
    int lsp_version;            /* Document version for LSP */
    /* Marks/bookmarks */
    sol_array(sol_position) marks;
    /* Search state */
    sol_array(sol_range) search_matches;
    int current_match;
};

SOL_API sol_buffer* sol_buffer_create(const char* name);
SOL_API sol_buffer* sol_buffer_open(const char* filepath);
SOL_API void sol_buffer_destroy(sol_buffer* buf);
SOL_API sol_result sol_buffer_save(sol_buffer* buf);
SOL_API sol_result sol_buffer_save_as(sol_buffer* buf, const char* filepath);
SOL_API sol_result sol_buffer_reload(sol_buffer* buf);
SOL_API void sol_buffer_insert(sol_buffer* buf, sol_position pos, const char* text, size_t len);
SOL_API void sol_buffer_delete(sol_buffer* buf, sol_range range);
SOL_API void sol_buffer_replace(sol_buffer* buf, sol_range range, const char* text, size_t len);
SOL_API size_t sol_buffer_get_line(sol_buffer* buf, int32_t line, char* out, size_t out_size);
SOL_API size_t sol_buffer_get_text(sol_buffer* buf, sol_range range, char* out, size_t out_size);
SOL_API int32_t sol_buffer_line_count(sol_buffer* buf);
SOL_API int32_t sol_buffer_line_length(sol_buffer* buf, int32_t line);
SOL_API size_t sol_buffer_length(sol_buffer* buf);
SOL_API bool sol_buffer_is_modified(sol_buffer* buf);
SOL_API const char* sol_buffer_get_language(sol_buffer* buf);

/* ============================================================================
 * Cursor and Selection
 * ============================================================================ */

typedef struct {
    sol_position pos;           /* Cursor position */
    sol_position anchor;        /* Selection anchor (same as pos if no selection) */
    int32_t preferred_column;   /* Preferred column for vertical movement */
} sol_cursor;

SOL_API bool sol_cursor_has_selection(sol_cursor* cursor);
SOL_API sol_range sol_cursor_get_selection(sol_cursor* cursor);
SOL_API void sol_cursor_clear_selection(sol_cursor* cursor);
SOL_API void sol_cursor_select_all(sol_cursor* cursor, sol_buffer* buf);
SOL_API void sol_cursor_select_word(sol_cursor* cursor, sol_buffer* buf);
SOL_API void sol_cursor_select_line(sol_cursor* cursor, sol_buffer* buf);

/* ============================================================================
 * View - A viewport into a buffer
 * ============================================================================ */

struct sol_view {
    uint32_t id;
    sol_buffer* buffer;
    sol_cursor cursor;
    int32_t scroll_line;        /* First visible line */
    int32_t scroll_col;         /* Horizontal scroll offset */
    int x, y, width, height;    /* View bounds in terminal */
    bool line_numbers;
    bool word_wrap;
    bool focused;
    /* Multiple cursors (for future) */
    sol_array(sol_cursor) extra_cursors;
};

SOL_API sol_view* sol_view_create(sol_buffer* buf);
SOL_API void sol_view_destroy(sol_view* view);
SOL_API void sol_view_set_buffer(sol_view* view, sol_buffer* buf);
SOL_API void sol_view_scroll_to_cursor(sol_view* view);
SOL_API void sol_view_scroll_to_line(sol_view* view, int32_t line);
SOL_API void sol_view_move_cursor(sol_view* view, int dx, int dy);
SOL_API void sol_view_move_cursor_to(sol_view* view, sol_position pos);
SOL_API void sol_view_page_up(sol_view* view);
SOL_API void sol_view_page_down(sol_view* view);
SOL_API void sol_view_cursor_up(sol_view* view);
SOL_API void sol_view_cursor_down(sol_view* view);
SOL_API void sol_view_cursor_left(sol_view* view);
SOL_API void sol_view_cursor_right(sol_view* view);
SOL_API sol_position sol_view_screen_to_buffer(sol_view* view, int screen_x, int screen_y);

/* ============================================================================
 * Keybinding System
 * ============================================================================ */

/* Modifier keys */
#define SOL_MOD_NONE    0x00
#define SOL_MOD_CTRL    0x01
#define SOL_MOD_ALT     0x02
#define SOL_MOD_SHIFT   0x04
#define SOL_MOD_SUPER   0x08    /* Cmd on macOS, Win on Windows */

/* Key codes (extends TUI key codes) */
typedef enum {
    SOL_KEY_NONE = 0,
    /* Printable ASCII range: 32-126 */
    /* Special keys start at 256 */
    SOL_KEY_UP = 256,
    SOL_KEY_DOWN,
    SOL_KEY_LEFT,
    SOL_KEY_RIGHT,
    SOL_KEY_ENTER,
    SOL_KEY_TAB,
    SOL_KEY_BACKSPACE,
    SOL_KEY_DELETE,
    SOL_KEY_ESCAPE,
    SOL_KEY_HOME,
    SOL_KEY_END,
    SOL_KEY_PAGE_UP,
    SOL_KEY_PAGE_DOWN,
    SOL_KEY_INSERT,
    SOL_KEY_F1, SOL_KEY_F2, SOL_KEY_F3, SOL_KEY_F4,
    SOL_KEY_F5, SOL_KEY_F6, SOL_KEY_F7, SOL_KEY_F8,
    SOL_KEY_F9, SOL_KEY_F10, SOL_KEY_F11, SOL_KEY_F12
} sol_key;

/* A single key chord (e.g., Ctrl+K) */
typedef struct {
    sol_key key;
    uint8_t mods;
} sol_keychord;

/* A key sequence (e.g., Ctrl+K Ctrl+C) */
#define SOL_MAX_KEY_SEQUENCE 4

typedef struct {
    sol_keychord chords[SOL_MAX_KEY_SEQUENCE];
    int length;
} sol_keysequence;

/* Keybinding action types */
typedef enum {
    SOL_ACTION_COMMAND,         /* Execute a named command */
    SOL_ACTION_LUA,             /* Execute Lua code (future) */
    SOL_ACTION_MACRO            /* Execute recorded macro */
} sol_action_type;

struct sol_keybind {
    sol_keysequence sequence;
    sol_action_type action_type;
    char* command;              /* Command name or Lua code */
    char* when;                 /* Context condition (e.g., "editorFocus") */
    bool enabled;
};

/* Keybinding manager */
typedef struct sol_keymap sol_keymap;

SOL_API sol_keymap* sol_keymap_create(void);
SOL_API void sol_keymap_destroy(sol_keymap* km);
SOL_API void sol_keymap_bind(sol_keymap* km, const char* keys, const char* command, const char* when);
SOL_API void sol_keymap_unbind(sol_keymap* km, const char* keys);
SOL_API const char* sol_keymap_lookup(sol_keymap* km, sol_keysequence* seq, const char* context);
SOL_API sol_keysequence sol_keymap_parse(const char* keys);
SOL_API void sol_keybind_defaults(sol_keymap* km);

/* Current key sequence state (for multi-key bindings like Ctrl+K Ctrl+C) */
typedef struct {
    sol_keysequence partial;
    int partial_len;
    bool waiting;
    uint64_t timeout_start;
} sol_key_state;

/* ============================================================================
 * Command System
 * ============================================================================ */

/* Command handler function type */
typedef void (*sol_command_fn)(sol_editor* ed, void* userdata);

struct sol_command {
    const char* id;             /* Unique command ID (e.g., "file.save") */
    const char* title;          /* Human-readable title */
    const char* description;
    sol_command_fn handler;
    void* userdata;
    bool enabled;
};

typedef struct sol_command_registry sol_command_registry;

SOL_API sol_command_registry* sol_command_registry_create(void);
SOL_API void sol_command_registry_destroy(sol_command_registry* reg);
SOL_API void sol_command_register(sol_command_registry* reg, sol_command* cmd);
SOL_API void sol_command_unregister(sol_command_registry* reg, const char* id);
SOL_API sol_command* sol_command_get(sol_command_registry* reg, const char* id);
SOL_API void sol_command_execute(sol_command_registry* reg, const char* id, sol_editor* ed);
SOL_API sol_array(sol_command*) sol_command_list(sol_command_registry* reg);
SOL_API sol_array(sol_command*) sol_command_search(sol_command_registry* reg, const char* query);
SOL_API void sol_commands_register_builtin(sol_command_registry* reg);

/* ============================================================================
 * File Tree
 * ============================================================================ */

typedef enum {
    SOL_FILE_NODE_FILE,
    SOL_FILE_NODE_DIRECTORY,
    SOL_FILE_NODE_SYMLINK
} sol_file_node_type;

typedef struct sol_file_node {
    char* name;
    char* path;
    sol_file_node_type type;
    struct sol_file_node* parent;
    struct sol_file_node** children;
    int child_count;
    int child_capacity;
    bool expanded;
    bool loaded;                /* Children loaded from disk */
    /* Git status */
    char git_status;            /* M, A, D, ?, etc. */
    /* For displaying */
    int depth;
    bool selected;
} sol_file_node;

struct sol_filetree {
    char* root_path;
    sol_file_node* root;
    sol_file_node* selected;
    int scroll_offset;
    int visible_height;
    /* Ignore patterns */
    sol_array(char*) ignore_patterns;
    /* Filtering */
    char* filter;
    bool show_hidden;
};

SOL_API sol_filetree* sol_filetree_create(const char* root_path);
SOL_API void sol_filetree_destroy(sol_filetree* tree);
SOL_API void sol_filetree_refresh(sol_filetree* tree);
SOL_API void sol_filetree_toggle_expand(sol_filetree* tree, sol_file_node* node);
SOL_API void sol_filetree_select_next(sol_filetree* tree);
SOL_API void sol_filetree_select_prev(sol_filetree* tree);
SOL_API sol_file_node* sol_filetree_get_visible(sol_filetree* tree, int index);
SOL_API int sol_filetree_visible_count(sol_filetree* tree);
SOL_API void sol_filetree_add_ignore(sol_filetree* tree, const char* pattern);

/* ============================================================================
 * Panel System
 * ============================================================================ */

typedef enum {
    SOL_PANEL_FILETREE,
    SOL_PANEL_EDITOR,
    SOL_PANEL_TERMINAL,
    SOL_PANEL_OUTPUT,
    SOL_PANEL_PROBLEMS,
    SOL_PANEL_SEARCH,
    SOL_PANEL_GIT,
    SOL_PANEL_CUSTOM
} sol_panel_type;

typedef enum {
    SOL_SPLIT_NONE,
    SOL_SPLIT_HORIZONTAL,
    SOL_SPLIT_VERTICAL
} sol_split_type;

struct sol_panel {
    uint32_t id;
    sol_panel_type type;
    char* title;
    int x, y, width, height;
    bool visible;
    bool focused;
    /* Content depending on type */
    union {
        sol_filetree* filetree;
        sol_view* view;
        sol_terminal* terminal;
        void* custom;
    } content;
    /* Split layout */
    sol_split_type split;
    struct sol_panel* child1;
    struct sol_panel* child2;
    float split_ratio;          /* 0.0 - 1.0 */
    struct sol_panel* parent;
};

/* ============================================================================
 * Window - A tab or workspace
 * ============================================================================ */

struct sol_window {
    uint32_t id;
    char* title;
    sol_panel* root_panel;
    sol_panel* focused_panel;
    sol_view* active_view;
};

/* ============================================================================
 * Theme
 * ============================================================================ */

struct sol_theme {
    char* name;
    /* Base colors */
    uint32_t bg;
    uint32_t fg;
    uint32_t fg_dim;
    uint32_t accent;
    uint32_t selection;
    uint32_t cursor;
    uint32_t cursor_line;
    /* Panel colors */
    uint32_t panel_bg;
    uint32_t panel_border;
    /* Widget colors */
    uint32_t widget_bg;
    uint32_t widget_fg;
    uint32_t select_bg;
    uint32_t select_fg;
    /* Status bar */
    uint32_t statusbar_bg;
    uint32_t statusbar_fg;
    /* Tab bar */
    uint32_t tabbar_bg;
    uint32_t tabbar_active;
    uint32_t tabbar_inactive;
    /* Line numbers */
    uint32_t line_number;
    uint32_t line_number_active;
    /* Syntax highlighting */
    uint32_t syntax_keyword;
    uint32_t syntax_type;
    uint32_t syntax_function;
    uint32_t syntax_variable;
    uint32_t syntax_string;
    uint32_t syntax_number;
    uint32_t syntax_comment;
    uint32_t syntax_operator;
    uint32_t syntax_punctuation;
    uint32_t syntax_preprocessor;
    uint32_t syntax_constant;
    uint32_t syntax_escape;
    /* Git colors */
    uint32_t git_added;
    uint32_t git_modified;
    uint32_t git_deleted;
    uint32_t git_conflict;
    /* Diagnostic colors */
    uint32_t error;
    uint32_t warning;
    uint32_t info;
    uint32_t hint;
};

SOL_API sol_theme* sol_theme_load(const char* path);
SOL_API sol_theme* sol_theme_default_dark(void);
SOL_API sol_theme* sol_theme_default_light(void);
SOL_API void sol_theme_destroy(sol_theme* theme);

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef enum {
    SOL_CONFIG_STRING,
    SOL_CONFIG_INT,
    SOL_CONFIG_FLOAT,
    SOL_CONFIG_BOOL,
    SOL_CONFIG_ARRAY,
    SOL_CONFIG_OBJECT
} sol_config_type;

typedef struct sol_config_value {
    sol_config_type type;
    union {
        char* string;
        int64_t integer;
        double number;
        bool boolean;
        struct {
            struct sol_config_value** items;
            int count;
        } array;
        sol_hashmap* object;
    } data;
} sol_config_value;

struct sol_config {
    sol_hashmap* values;
    char* path;                 /* Config file path */
    bool modified;
};

SOL_API sol_config* sol_config_create(void);
SOL_API void sol_config_load_file(sol_config* cfg, const char* path);
SOL_API sol_config* sol_config_load(const char* path);
SOL_API void sol_config_destroy(sol_config* cfg);
SOL_API sol_result sol_config_save(sol_config* cfg);
SOL_API sol_config_value* sol_config_get(sol_config* cfg, const char* key);
SOL_API void sol_config_set(sol_config* cfg, const char* key, sol_config_value* value);
SOL_API const char* sol_config_get_string(sol_config* cfg, const char* key, const char* default_val);
SOL_API int64_t sol_config_get_int(sol_config* cfg, const char* key, int64_t default_val);
SOL_API bool sol_config_get_bool(sol_config* cfg, const char* key, bool default_val);
SOL_API int sol_config_get_tab_width(sol_config* cfg);
SOL_API sol_theme* sol_config_get_theme(sol_config* cfg);
SOL_API void sol_config_add_recent_file(sol_config* cfg, const char* path);

/* ============================================================================
 * Plugin System
 * ============================================================================ */

#define SOL_PLUGIN_API_VERSION 1

typedef struct sol_plugin_api {
    int api_version;
    /* Editor access */
    sol_editor* (*get_editor)(void);
    sol_buffer* (*get_active_buffer)(void);
    sol_view* (*get_active_view)(void);
    /* Buffer operations */
    void (*buffer_insert)(sol_buffer* buf, sol_position pos, const char* text);
    void (*buffer_delete)(sol_buffer* buf, sol_range range);
    /* Commands */
    void (*register_command)(const char* id, const char* title, sol_command_fn fn, void* userdata);
    void (*execute_command)(const char* id);
    /* Keybindings */
    void (*bind_key)(const char* keys, const char* command);
    /* UI */
    void (*message)(const char* msg);
    void (*status)(const char* msg);
    /* Config */
    const char* (*config_get_string)(const char* key, const char* default_val);
    int64_t (*config_get_int)(const char* key, int64_t default_val);
} sol_plugin_api;

typedef struct {
    const char* name;
    const char* version;
    const char* description;
    const char* author;
    int api_version;
    /* Lifecycle hooks */
    int (*init)(sol_plugin_api* api);
    void (*shutdown)(void);
    void (*on_buffer_open)(sol_buffer* buf);
    void (*on_buffer_save)(sol_buffer* buf);
    void (*on_buffer_close)(sol_buffer* buf);
} sol_plugin_info;

struct sol_plugin {
    sol_plugin_info* info;
    void* handle;               /* DLL/SO handle */
    char* path;
    bool loaded;
    bool enabled;
};

typedef struct sol_plugin_manager sol_plugin_manager;

SOL_API sol_plugin_manager* sol_plugin_manager_create(sol_editor* ed);
SOL_API void sol_plugin_manager_destroy(sol_plugin_manager* pm);
SOL_API sol_result sol_plugin_load(sol_plugin_manager* pm, const char* path);
SOL_API void sol_plugin_unload(sol_plugin_manager* pm, sol_plugin* plugin);
SOL_API sol_array(sol_plugin*) sol_plugin_list(sol_plugin_manager* pm);
SOL_API void sol_plugins_shutdown(sol_plugin_manager* pm);

/* Plugin entry point - must be exported by plugins */
#define SOL_PLUGIN_ENTRY sol_plugin_info* sol_plugin_get_info(void)

/* ============================================================================
 * LSP Client
 * ============================================================================ */

typedef enum {
    SOL_LSP_STOPPED,
    SOL_LSP_STARTING,
    SOL_LSP_RUNNING,
    SOL_LSP_SHUTTING_DOWN
} sol_lsp_state;

typedef struct {
    int32_t line;
    int32_t character;
} sol_lsp_position;

/* ============================================================================
 * Syntax Highlighting
 * ============================================================================ */

struct sol_highlighter;  /* Opaque */

SOL_API struct sol_highlighter* sol_highlighter_create(const char* filepath);
SOL_API void sol_highlighter_destroy(struct sol_highlighter* hl);
SOL_API void sol_highlight_line(struct sol_highlighter* hl, const char* line, size_t len, uint8_t* token_types);
SOL_API void sol_highlighter_reset(struct sol_highlighter* hl);

/* ============================================================================
 * LSP Types (continued)
 * ============================================================================ */

typedef struct {
    char* label;
    char* detail;
    char* documentation;
    int kind;                   /* CompletionItemKind */
    char* insert_text;
} sol_lsp_completion_item;

typedef struct {
    char* message;
    int severity;               /* 1=Error, 2=Warning, 3=Info, 4=Hint */
    sol_range range;
    char* source;
    char* code;
} sol_lsp_diagnostic;

typedef void (*sol_lsp_completion_callback)(sol_lsp_completion_item* items, int count, void* userdata);
typedef void (*sol_lsp_diagnostic_callback)(sol_lsp_diagnostic* diags, int count, void* userdata);

struct sol_lsp_client {
    char* language_id;
    char* command;              /* LSP server command */
    sol_lsp_state state;
    /* Process handle */
    void* process;
    int stdin_fd;
    int stdout_fd;
    /* Message handling */
    int next_id;
    sol_hashmap* pending_requests;
    /* Callbacks */
    sol_lsp_diagnostic_callback on_diagnostics;
    void* userdata;
};

SOL_API sol_lsp_client* sol_lsp_client_create(const char* language_id, const char* command);
SOL_API void sol_lsp_client_destroy(sol_lsp_client* client);
SOL_API sol_result sol_lsp_start(sol_lsp_client* client, const char* root_path);
SOL_API void sol_lsp_stop(sol_lsp_client* client);
SOL_API void sol_lsp_did_open(sol_lsp_client* client, sol_buffer* buf);
SOL_API void sol_lsp_did_change(sol_lsp_client* client, sol_buffer* buf, sol_range range, const char* text);
SOL_API void sol_lsp_did_save(sol_lsp_client* client, sol_buffer* buf);
SOL_API void sol_lsp_did_close(sol_lsp_client* client, sol_buffer* buf);
SOL_API void sol_lsp_update(sol_lsp_client* client);
SOL_API void sol_lsp_completion(sol_lsp_client* client, sol_buffer* buf, sol_position pos, 
                                 sol_lsp_completion_callback callback, void* userdata);
SOL_API void sol_lsp_hover(sol_lsp_client* client, sol_buffer* buf, sol_position pos);
SOL_API void sol_lsp_goto_definition(sol_lsp_client* client, sol_buffer* buf, sol_position pos);
SOL_API void sol_lsp_find_references(sol_lsp_client* client, sol_buffer* buf, sol_position pos);
SOL_API void sol_lsp_poll(sol_lsp_client* client);  /* Process incoming messages */

/* ============================================================================
 * Terminal Emulator
 * ============================================================================ */

typedef enum {
    SOL_TERM_STATE_NORMAL,
    SOL_TERM_STATE_ESC,
    SOL_TERM_STATE_CSI,
    SOL_TERM_STATE_OSC
} sol_term_state;

typedef struct {
    uint32_t ch;
    uint32_t fg;
    uint32_t bg;
    uint8_t style;
} sol_term_cell;

/* Terminal structure is opaque - defined in terminal.c */

SOL_API sol_terminal* sol_terminal_create(int width, int height);
SOL_API void sol_terminal_destroy(sol_terminal* term);
SOL_API sol_result sol_terminal_spawn(sol_terminal* term, const char* shell);
SOL_API void sol_terminal_resize(sol_terminal* term, int width, int height);
SOL_API void sol_terminal_write(sol_terminal* term, const char* data, size_t len);
SOL_API void sol_terminal_process_output(sol_terminal* term);
SOL_API void sol_terminal_scroll(sol_terminal* term, int lines);
SOL_API bool sol_terminal_is_running(sol_terminal* term);
SOL_API void sol_terminal_update(sol_terminal* term);

/* ============================================================================
 * Git Integration
 * ============================================================================ */

typedef enum {
    SOL_GIT_STATUS_UNMODIFIED = ' ',
    SOL_GIT_STATUS_MODIFIED = 'M',
    SOL_GIT_STATUS_ADDED = 'A',
    SOL_GIT_STATUS_DELETED = 'D',
    SOL_GIT_STATUS_RENAMED = 'R',
    SOL_GIT_STATUS_COPIED = 'C',
    SOL_GIT_STATUS_UNTRACKED = '?',
    SOL_GIT_STATUS_IGNORED = '!'
} sol_git_status;

typedef struct {
    char* path;
    sol_git_status index_status;
    sol_git_status worktree_status;
} sol_git_file;

typedef struct {
    int32_t line;               /* 0-indexed */
    sol_git_status status;      /* Type of change */
    int32_t old_line;           /* For context, -1 if new */
} sol_git_hunk;

struct sol_git {
    char* repo_path;
    char* branch;
    char* remote;
    /* Status */
    sol_array(sol_git_file) status;
    /* Per-buffer diff hunks */
    sol_hashmap* buffer_hunks;  /* buffer_id -> sol_array(sol_git_hunk) */
};

SOL_API sol_git* sol_git_open(const char* path);
SOL_API void sol_git_close(sol_git* git);
SOL_API void sol_git_refresh(sol_git* git);
SOL_API sol_array(sol_git_hunk)* sol_git_get_hunks(sol_git* git, sol_buffer* buf);
SOL_API const char* sol_git_get_branch(sol_git* git);
SOL_API int sol_git_ahead_behind(sol_git* git, int* ahead, int* behind);
SOL_API sol_result sol_git_stage_file(sol_git* git, const char* path);
SOL_API sol_result sol_git_unstage_file(sol_git* git, const char* path);
SOL_API sol_result sol_git_commit(sol_git* git, const char* message);

/* ============================================================================
 * Search
 * ============================================================================ */

typedef struct {
    char* path;
    int32_t line;
    int32_t column;
    char* match_text;           /* The matched text */
    char* context;              /* Surrounding context */
} sol_search_result;

typedef struct {
    char* query;
    bool regex;
    bool case_sensitive;
    bool whole_word;
    char* include_pattern;      /* Glob pattern for files to include */
    char* exclude_pattern;      /* Glob pattern for files to exclude */
    sol_array(sol_search_result) results;
    bool in_progress;
    int files_searched;
    int matches_found;
} sol_search;

SOL_API sol_search* sol_search_create(const char* query);
SOL_API void sol_search_destroy(sol_search* search);
SOL_API void sol_search_in_buffer(sol_search* search, sol_buffer* buf);
SOL_API void sol_search_in_directory(sol_search* search, const char* path);
SOL_API void sol_search_cancel(sol_search* search);

/* ============================================================================
 * Main Editor State
 * ============================================================================ */

struct sol_editor {
    /* Core state */
    bool running;
    bool dirty;                 /* UI needs redraw */
    /* Buffers */
    sol_array(sol_buffer*) buffers;
    sol_buffer* active_buffer;
    /* Views */
    sol_array(sol_view*) views;
    sol_view* active_view;
    /* Windows/Tabs */
    sol_array(sol_window*) windows;
    sol_window* active_window;
    /* Panels */
    sol_panel* root_panel;
    sol_panel* focused_panel;
    sol_filetree* filetree;
    /* UI state */
    int term_width;
    int term_height;
    int sidebar_width;
    int statusbar_height;
    int tabbar_height;
    bool sidebar_visible;
    bool terminal_visible;
    int terminal_height;
    /* Subsystems */
    sol_command_registry* commands;
    sol_keymap* keymap;
    sol_key_state key_state;
    sol_config* config;
    sol_theme* theme;
    sol_plugin_manager* plugins;
    sol_git* git;
    sol_hashmap* lsp_clients;   /* language_id -> sol_lsp_client* */
    /* Terminal */
    sol_array(sol_terminal*) terminals;
    sol_terminal* active_terminal;
    /* Message/Status */
    char status_message[256];
    uint64_t status_time;
    /* Command palette */
    bool palette_open;
    char palette_query[256];
    int palette_selection;
    sol_array(sol_command*) palette_results;
    /* Find dialog */
    bool find_open;
    char find_query[256];
    char replace_query[256];
    int find_selection;
    /* File picker */
    bool file_picker_open;
    bool file_picker_select_folder;  /* true = select folder, false = select file */
    /* Arena for temporary allocations per frame */
    sol_arena* frame_arena;
    /* TUI context */
    void* tui;
};

/* ============================================================================
 * Editor Lifecycle
 * ============================================================================ */

SOL_API sol_editor* sol_editor_create(void);
SOL_API void sol_editor_destroy(sol_editor* ed);
SOL_API void sol_editor_run(sol_editor* ed);
SOL_API void sol_editor_quit(sol_editor* ed);

/* Workspace management */
SOL_API void sol_editor_open_workspace(sol_editor* ed, const char* path);

/* Buffer management */
SOL_API sol_buffer* sol_editor_open_file(sol_editor* ed, const char* path);
SOL_API sol_buffer* sol_editor_new_file(sol_editor* ed);
SOL_API void sol_editor_close_buffer(sol_editor* ed, sol_buffer* buf);
SOL_API void sol_editor_save_buffer(sol_editor* ed, sol_buffer* buf);
SOL_API void sol_editor_save_all(sol_editor* ed);

/* View management */
SOL_API sol_view* sol_editor_create_view(sol_editor* ed, sol_buffer* buf);
SOL_API void sol_editor_split_view(sol_editor* ed, bool vertical);
SOL_API void sol_editor_close_view(sol_editor* ed, sol_view* view);
SOL_API void sol_editor_focus_view(sol_editor* ed, sol_view* view);

/* UI */
SOL_API void sol_editor_message(sol_editor* ed, const char* fmt, ...);
SOL_API void sol_editor_status(sol_editor* ed, const char* fmt, ...);
SOL_API void sol_editor_toggle_sidebar(sol_editor* ed);
SOL_API void sol_editor_toggle_terminal(sol_editor* ed);
SOL_API void sol_editor_open_palette(sol_editor* ed);
SOL_API void sol_editor_open_find(sol_editor* ed);
SOL_API void sol_editor_open_file_picker(sol_editor* ed);
SOL_API void sol_editor_open_folder_picker(sol_editor* ed);

/* File picker */
SOL_API void sol_file_picker_draw(tui_context* tui, sol_editor* ed);
SOL_API bool sol_file_picker_handle_key(sol_editor* ed, tui_event* event);
SOL_API void sol_file_picker_close(sol_editor* ed);

/* Dialog utilities */
SOL_API void sol_dialog_message(tui_context* tui, sol_theme* theme, 
                                const char* title, const char* message);
SOL_API void sol_dialog_input(tui_context* tui, sol_theme* theme,
                              const char* title, const char* prompt,
                              void (*callback)(const char*, void*), void* userdata);
SOL_API void sol_dialog_input_draw(tui_context* tui, sol_theme* theme, const char* title);
SOL_API bool sol_dialog_input_handle_key(tui_event* event);
SOL_API bool sol_dialog_is_active(void);

/* Command palette */
SOL_API void sol_palette_draw(tui_context* tui, sol_editor* ed);
SOL_API void sol_palette_handle_key(sol_editor* ed, tui_event* event);

/* Tab bar */
SOL_API void sol_tabbar_draw(tui_context* tui, sol_editor* ed, int x, int y, int width);

/* Status bar */
SOL_API void sol_statusbar_draw(tui_context* tui, sol_editor* ed, int x, int y, int width);

/* Editor view rendering */
SOL_API void sol_editor_view_draw(tui_context* tui, sol_view* view, sol_theme* theme);

/* Terminal UI */
SOL_API void sol_terminal_draw(tui_context* tui, sol_terminal* term, sol_theme* theme,
                               int x, int y, int w, int h);
SOL_API void sol_terminal_handle_key(sol_terminal* term, tui_event* event);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* String utilities */
SOL_API char* sol_strdup(const char* str);
SOL_API char* sol_strndup(const char* str, size_t len);
SOL_API bool sol_str_starts_with(const char* str, const char* prefix);
SOL_API bool sol_str_ends_with(const char* str, const char* suffix);
SOL_API char* sol_str_trim(char* str);
SOL_API int sol_utf8_decode(const char* str, uint32_t* out);
SOL_API int sol_utf8_encode(uint32_t cp, char* out);
SOL_API size_t sol_utf8_strlen(const char* str);

/* Path utilities */
SOL_API char* sol_path_join(const char* base, const char* path);
SOL_API char* sol_path_dirname(const char* path);
SOL_API const char* sol_path_basename(const char* path);
SOL_API const char* sol_path_extension(const char* path);
SOL_API char* sol_path_normalize(const char* path);
SOL_API bool sol_path_exists(const char* path);
SOL_API bool sol_path_is_directory(const char* path);
SOL_API bool sol_path_is_file(const char* path);
SOL_API char* sol_path_home(void);
SOL_API char* sol_path_config_dir(void);

/* File I/O */
SOL_API char* sol_file_read(const char* path, size_t* out_len);
SOL_API sol_result sol_file_write(const char* path, const char* data, size_t len);
SOL_API uint64_t sol_file_mtime(const char* path);
SOL_API sol_array(char*) sol_dir_list(const char* path);

/* Time */
SOL_API uint64_t sol_time_ms(void);
SOL_API uint64_t sol_time_us(void);

/* Glob/Pattern matching */
SOL_API bool sol_glob_match(const char* pattern, const char* str);

/* Fuzzy matching (for command palette, file picker) */
SOL_API int sol_fuzzy_match(const char* pattern, const char* str, int* out_score);

#ifdef __cplusplus
}
#endif

#endif /* SOL_H */
