/*
 * Sol IDE - File Tree Implementation
 */

#include "tui.h"
#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Static scroll position for filetree */
static int s_filetree_scroll = 0;

/* Sort comparison for file nodes */
static int compare_nodes(const void* a, const void* b) {
    sol_file_node* na = *(sol_file_node**)a;
    sol_file_node* nb = *(sol_file_node**)b;
    
    /* Directories first */
    if (na->type == SOL_FILE_NODE_DIRECTORY && nb->type != SOL_FILE_NODE_DIRECTORY) {
        return -1;
    }
    if (na->type != SOL_FILE_NODE_DIRECTORY && nb->type == SOL_FILE_NODE_DIRECTORY) {
        return 1;
    }
    
    /* Then alphabetically (case-insensitive) */
    return strcasecmp(na->name, nb->name);
}

/* Check if a file should be ignored */
static bool should_ignore(sol_filetree* tree, const char* name) {
    if (!name) return true;
    
    /* Always ignore . and .. */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }
    
    /* Check hidden files if not showing them */
    if (!tree->show_hidden && name[0] == '.') {
        return true;
    }
    
    /* Check ignore patterns */
    for (size_t i = 0; i < sol_array_count(tree->ignore_patterns); i++) {
        if (sol_glob_match(tree->ignore_patterns[i], name)) {
            return true;
        }
    }
    
    return false;
}

/* Create a file node */
static sol_file_node* node_create(const char* name, const char* path, sol_file_node_type type) {
    sol_file_node* node = (sol_file_node*)calloc(1, sizeof(sol_file_node));
    if (!node) return NULL;
    
    node->name = sol_strdup(name);
    node->path = sol_strdup(path);
    node->type = type;
    
    return node;
}

/* Destroy a file node and its children */
static void node_destroy(sol_file_node* node) {
    if (!node) return;
    
    for (int i = 0; i < node->child_count; i++) {
        node_destroy(node->children[i]);
    }
    
    free(node->children);
    free(node->name);
    free(node->path);
    free(node);
}

/* Add child to node */
static void node_add_child(sol_file_node* parent, sol_file_node* child) {
    if (parent->child_count >= parent->child_capacity) {
        int new_cap = parent->child_capacity == 0 ? 16 : parent->child_capacity * 2;
        sol_file_node** new_children = (sol_file_node**)realloc(
            parent->children,
            (size_t)new_cap * sizeof(sol_file_node*)
        );
        if (!new_children) return;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    child->depth = parent->depth + 1;
}

/* Load children of a directory node */
static void node_load_children(sol_filetree* tree, sol_file_node* node) {
    if (!node || node->type != SOL_FILE_NODE_DIRECTORY || node->loaded) {
        return;
    }
    
    sol_array(char*) entries = sol_dir_list(node->path);
    if (!entries) {
        node->loaded = true;
        return;
    }
    
    for (size_t i = 0; i < sol_array_count(entries); i++) {
        char* entry = entries[i];
        
        /* Check if directory (ends with /) */
        size_t len = strlen(entry);
        bool is_dir = len > 0 && entry[len - 1] == '/';
        if (is_dir) {
            entry[len - 1] = '\0';  /* Remove trailing / */
        }
        
        if (should_ignore(tree, entry)) {
            free(entries[i]);
            continue;
        }
        
        char* full_path = sol_path_join(node->path, entry);
        if (!full_path) {
            free(entries[i]);
            continue;
        }
        
        sol_file_node_type type = is_dir ? SOL_FILE_NODE_DIRECTORY : SOL_FILE_NODE_FILE;
        sol_file_node* child = node_create(entry, full_path, type);
        
        free(full_path);
        free(entries[i]);
        
        if (child) {
            node_add_child(node, child);
        }
    }
    
    sol_array_free(entries);
    
    /* Sort children */
    if (node->child_count > 1) {
        qsort(node->children, (size_t)node->child_count, sizeof(sol_file_node*), compare_nodes);
    }
    
    node->loaded = true;
}

sol_filetree* sol_filetree_create(const char* root_path) {
    sol_filetree* tree = (sol_filetree*)calloc(1, sizeof(sol_filetree));
    if (!tree) return NULL;
    
    tree->root_path = sol_strdup(root_path);
    tree->show_hidden = false;
    
    /* Default ignore patterns */
    sol_filetree_add_ignore(tree, "*.o");
    sol_filetree_add_ignore(tree, "*.obj");
    sol_filetree_add_ignore(tree, "*.exe");
    sol_filetree_add_ignore(tree, "*.dll");
    sol_filetree_add_ignore(tree, "*.so");
    sol_filetree_add_ignore(tree, "*.dylib");
    sol_filetree_add_ignore(tree, "*.pyc");
    sol_filetree_add_ignore(tree, "__pycache__");
    sol_filetree_add_ignore(tree, "node_modules");
    sol_filetree_add_ignore(tree, ".git");
    sol_filetree_add_ignore(tree, ".svn");
    sol_filetree_add_ignore(tree, ".hg");
    sol_filetree_add_ignore(tree, ".DS_Store");
    sol_filetree_add_ignore(tree, "Thumbs.db");
    
    /* Create root node */
    tree->root = node_create(sol_path_basename(root_path), root_path, SOL_FILE_NODE_DIRECTORY);
    if (tree->root) {
        tree->root->expanded = true;
        node_load_children(tree, tree->root);
        tree->selected = tree->root;
    }
    
    return tree;
}

void sol_filetree_destroy(sol_filetree* tree) {
    if (!tree) return;
    
    node_destroy(tree->root);
    free(tree->root_path);
    free(tree->filter);
    
    for (size_t i = 0; i < sol_array_count(tree->ignore_patterns); i++) {
        free(tree->ignore_patterns[i]);
    }
    sol_array_free(tree->ignore_patterns);
    
    free(tree);
}

void sol_filetree_refresh(sol_filetree* tree) {
    if (!tree || !tree->root) return;
    
    /* Clear and reload */
    for (int i = 0; i < tree->root->child_count; i++) {
        node_destroy(tree->root->children[i]);
    }
    tree->root->child_count = 0;
    tree->root->loaded = false;
    
    node_load_children(tree, tree->root);
}

void sol_filetree_toggle_expand(sol_filetree* tree, sol_file_node* node) {
    if (!tree || !node || node->type != SOL_FILE_NODE_DIRECTORY) {
        return;
    }
    
    node->expanded = !node->expanded;
    
    if (node->expanded && !node->loaded) {
        node_load_children(tree, node);
    }
}

/* Count visible nodes recursively */
static int count_visible(sol_file_node* node) {
    if (!node) return 0;
    
    int count = 1;  /* This node */
    
    if (node->type == SOL_FILE_NODE_DIRECTORY && node->expanded) {
        for (int i = 0; i < node->child_count; i++) {
            count += count_visible(node->children[i]);
        }
    }
    
    return count;
}

int sol_filetree_visible_count(sol_filetree* tree) {
    if (!tree || !tree->root) return 0;
    return count_visible(tree->root);
}

/* Get visible node at index */
static sol_file_node* get_visible_at(sol_file_node* node, int* index) {
    if (!node || *index < 0) return NULL;
    
    if (*index == 0) {
        return node;
    }
    (*index)--;
    
    if (node->type == SOL_FILE_NODE_DIRECTORY && node->expanded) {
        for (int i = 0; i < node->child_count; i++) {
            sol_file_node* result = get_visible_at(node->children[i], index);
            if (result) return result;
        }
    }
    
    return NULL;
}

sol_file_node* sol_filetree_get_visible(sol_filetree* tree, int index) {
    if (!tree || !tree->root || index < 0) return NULL;
    return get_visible_at(tree->root, &index);
}

/* Find index of a node */
static int find_node_index(sol_file_node* root, sol_file_node* target, int* index) {
    if (!root || !target) return -1;
    
    if (root == target) {
        return *index;
    }
    (*index)++;
    
    if (root->type == SOL_FILE_NODE_DIRECTORY && root->expanded) {
        for (int i = 0; i < root->child_count; i++) {
            int result = find_node_index(root->children[i], target, index);
            if (result >= 0) return result;
        }
    }
    
    return -1;
}

void sol_filetree_select_next(sol_filetree* tree) {
    if (!tree || !tree->root || !tree->selected) return;
    
    int index = 0;
    int current = find_node_index(tree->root, tree->selected, &index);
    if (current < 0) return;
    
    int visible = sol_filetree_visible_count(tree);
    if (current < visible - 1) {
        tree->selected = sol_filetree_get_visible(tree, current + 1);
    }
}

void sol_filetree_select_prev(sol_filetree* tree) {
    if (!tree || !tree->root || !tree->selected) return;
    
    int index = 0;
    int current = find_node_index(tree->root, tree->selected, &index);
    if (current < 0) return;
    
    if (current > 0) {
        tree->selected = sol_filetree_get_visible(tree, current - 1);
    }
}

void sol_filetree_add_ignore(sol_filetree* tree, const char* pattern) {
    if (!tree || !pattern) return;
    sol_array_push(tree->ignore_patterns, sol_strdup(pattern));
}

/* Get file icon based on extension */
static const char* get_file_icon(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return " ";  /* nf-fa-file_o */
    
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return " ";
    if (strcmp(ext, ".py") == 0) return " ";
    if (strcmp(ext, ".js") == 0) return " ";
    if (strcmp(ext, ".ts") == 0) return " ";
    if (strcmp(ext, ".rs") == 0) return " ";
    if (strcmp(ext, ".go") == 0) return " ";
    if (strcmp(ext, ".md") == 0) return " ";
    if (strcmp(ext, ".json") == 0) return " ";
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return " ";
    if (strcmp(ext, ".txt") == 0) return " ";
    if (strcmp(ext, ".sh") == 0) return " ";
    if (strcmp(ext, ".lua") == 0) return " ";
    
    return " ";  /* Default file icon */
}

/* Draw file tree */
void sol_filetree_draw(void* tui_ctx, sol_editor* ed, int x, int y, int width, int height) {
    if (!tui_ctx || !ed) return;
    
    tui_context* tui = (tui_context*)tui_ctx;
    sol_theme* theme = ed->theme;
    if (!theme) theme = sol_theme_default_dark();
    sol_filetree* tree = ed->filetree;
    
    /* Clear sidebar area */
    tui_set_bg(tui, theme->panel_bg);
    tui_set_fg(tui, theme->fg);
    tui_fill(tui, x, y, width, height, ' ');
    
    /* Draw header with focus indicator */
    if (ed->sidebar_focused) {
        tui_set_bg(tui, theme->accent);
        tui_set_fg(tui, theme->panel_bg);
        tui_fill(tui, x, y, width, 1, ' ');
    } else {
        tui_set_fg(tui, theme->accent);
    }
    const char* title = ed->sidebar_focused ? " EXPLORER (focused)" : " EXPLORER";
    tui_label(tui, x, y, title);
    tui_set_bg(tui, theme->panel_bg);
    
    if (!tree || !tree->root) {
        tui_set_fg(tui, theme->syntax_comment);
        tui_label(tui, x + 1, y + 2, "No folder open");
        tui_label(tui, x + 1, y + 3, "Ctrl+Space O to open");
        return;
    }
    
    /* Draw folder name */
    tui_set_fg(tui, theme->fg);
    char folder_name[64];
    snprintf(folder_name, sizeof(folder_name), " %s", tree->root->name);
    if ((int)strlen(folder_name) > width - 2) {
        folder_name[width - 5] = '.';
        folder_name[width - 4] = '.';
        folder_name[width - 3] = '.';
        folder_name[width - 2] = '\0';
    }
    tui_label(tui, x + 1, y + 1, folder_name);
    
    /* Calculate visible area */
    int content_y = y + 2;
    int content_height = height - 2;
    
    int visible_count = sol_filetree_visible_count(tree);
    
    /* Adjust scroll if selected is out of view */
    if (tree->selected) {
        int idx = 0;
        int selected_idx = find_node_index(tree->root, tree->selected, &idx);
        if (selected_idx >= 0) {
            if (selected_idx < s_filetree_scroll) {
                s_filetree_scroll = selected_idx;
            } else if (selected_idx >= s_filetree_scroll + content_height) {
                s_filetree_scroll = selected_idx - content_height + 1;
            }
        }
    }
    
    /* Clamp scroll */
    if (s_filetree_scroll > visible_count - content_height) {
        s_filetree_scroll = visible_count - content_height;
    }
    if (s_filetree_scroll < 0) s_filetree_scroll = 0;
    
    /* Draw visible nodes */
    for (int i = 0; i < content_height; i++) {
        int node_idx = i + s_filetree_scroll;
        if (node_idx >= visible_count) break;
        
        sol_file_node* node = sol_filetree_get_visible(tree, node_idx);
        if (!node) continue;
        
        int row_y = content_y + i;
        
        /* Highlight selected row */
        if (node == tree->selected) {
            tui_set_bg(tui, theme->selection);
            tui_fill(tui, x, row_y, width, 1, ' ');
        } else {
            tui_set_bg(tui, theme->panel_bg);
        }
        
        /* Indentation */
        int indent = node->depth * 2;
        int text_x = x + 1 + indent;
        
        /* Draw expand/collapse indicator for directories */
        if (node->type == SOL_FILE_NODE_DIRECTORY) {
            tui_set_fg(tui, theme->syntax_comment);
            const char* arrow = node->expanded ? "" : "";  /* nf-fa-angle_down/right */
            tui_label(tui, text_x, row_y, arrow);
            text_x += 2;
            
            /* Folder icon */
            tui_set_fg(tui, theme->syntax_function);
            const char* folder_icon = node->expanded ? " " : " ";  /* nf-fa-folder_open/folder */
            tui_label(tui, text_x, row_y, folder_icon);
            text_x += 2;
        } else {
            /* File icon */
            text_x += 2;  /* Skip arrow space */
            tui_set_fg(tui, theme->syntax_string);
            tui_label(tui, text_x, row_y, get_file_icon(node->name));
            text_x += 2;
        }
        
        /* Draw name */
        if (node->type == SOL_FILE_NODE_DIRECTORY) {
            tui_set_fg(tui, theme->syntax_function);
        } else {
            tui_set_fg(tui, theme->fg);
        }
        
        /* Truncate name if too long */
        int max_name_len = width - (text_x - x) - 1;
        if (max_name_len > 0) {
            char name_buf[256];
            if ((int)strlen(node->name) > max_name_len) {
                snprintf(name_buf, sizeof(name_buf), "%.*s...", max_name_len - 3, node->name);
            } else {
                strncpy(name_buf, node->name, sizeof(name_buf) - 1);
                name_buf[sizeof(name_buf) - 1] = '\0';
            }
            tui_label(tui, text_x, row_y, name_buf);
        }
    }
    
    /* Reset background */
    tui_set_bg(tui, theme->bg);
}

/* Handle filetree keyboard input */
bool sol_filetree_handle_key(sol_editor* ed, void* event_ptr) {
    if (!ed || !ed->filetree || !event_ptr) return false;
    
    tui_event* event = (tui_event*)event_ptr;
    if (event->type != TUI_EVENT_KEY) return false;
    
    sol_filetree* tree = ed->filetree;
    
    switch (event->key) {
        case TUI_KEY_UP:
            sol_filetree_select_prev(tree);
            return true;
            
        case TUI_KEY_DOWN:
            sol_filetree_select_next(tree);
            return true;
            
        case TUI_KEY_LEFT:
            /* Collapse directory or go to parent */
            if (tree->selected) {
                if (tree->selected->type == SOL_FILE_NODE_DIRECTORY && tree->selected->expanded) {
                    tree->selected->expanded = false;
                } else if (tree->selected->parent) {
                    tree->selected = tree->selected->parent;
                }
            }
            return true;
            
        case TUI_KEY_RIGHT:
            /* Expand directory */
            if (tree->selected && tree->selected->type == SOL_FILE_NODE_DIRECTORY) {
                if (!tree->selected->expanded) {
                    sol_filetree_toggle_expand(tree, tree->selected);
                } else if (tree->selected->child_count > 0) {
                    /* Move to first child */
                    tree->selected = tree->selected->children[0];
                }
            }
            return true;
            
        case TUI_KEY_ENTER:
            if (tree->selected) {
                if (tree->selected->type == SOL_FILE_NODE_DIRECTORY) {
                    sol_filetree_toggle_expand(tree, tree->selected);
                } else {
                    /* Open file and switch focus to editor */
                    sol_editor_open_file(ed, tree->selected->path);
                    ed->sidebar_focused = false;
                }
            }
            return true;
        
        case TUI_KEY_ESC:
            /* Switch focus back to editor */
            ed->sidebar_focused = false;
            return true;
            
        default:
            break;
    }
    
    return false;
}
