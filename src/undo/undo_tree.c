/*
 * Sol IDE - Undo/Redo Tree Implementation
 * 
 * Unlike a simple undo stack, an undo tree preserves all edit history.
 * When you undo and then make a new edit, a new branch is created
 * instead of discarding the previous future. This allows exploring
 * all possible edit histories.
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

sol_undo_tree* sol_undo_tree_create(void) {
    sol_undo_tree* tree = (sol_undo_tree*)calloc(1, sizeof(sol_undo_tree));
    if (!tree) return NULL;
    
    tree->arena = sol_arena_create();
    if (!tree->arena) {
        free(tree);
        return NULL;
    }
    
    /* Create root node (represents initial state) */
    tree->root = (sol_undo_node*)sol_arena_alloc_zero(tree->arena, sizeof(sol_undo_node));
    if (!tree->root) {
        sol_arena_destroy(tree->arena);
        free(tree);
        return NULL;
    }
    
    tree->root->id = tree->next_id++;
    tree->current = tree->root;
    
    return tree;
}

void sol_undo_tree_destroy(sol_undo_tree* tree) {
    if (!tree) return;
    
    /* Free edit texts (allocated with malloc, not arena) */
    /* Walk all nodes... for now just destroy arena */
    sol_arena_destroy(tree->arena);
    free(tree);
}

static sol_undo_node* create_node(sol_undo_tree* tree, sol_edit* edit) {
    sol_undo_node* node = (sol_undo_node*)sol_arena_alloc_zero(tree->arena, sizeof(sol_undo_node));
    if (!node) return NULL;
    
    node->id = tree->next_id++;
    
    /* Copy edit */
    node->edit = *edit;
    
    /* Make copies of text (these need to persist) */
    if (edit->old_text) {
        node->edit.old_text = sol_arena_strdup(tree->arena, edit->old_text);
    }
    if (edit->new_text) {
        node->edit.new_text = sol_arena_strdup(tree->arena, edit->new_text);
    }
    
    return node;
}

static void add_child(sol_undo_node* parent, sol_undo_node* child) {
    if (parent->child_count >= parent->child_capacity) {
        int new_cap = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        sol_undo_node** new_children = (sol_undo_node**)realloc(
            parent->children, 
            (size_t)new_cap * sizeof(sol_undo_node*)
        );
        if (!new_children) return;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    
    parent->children[parent->child_count] = child;
    parent->active_child = parent->child_count;
    parent->child_count++;
    child->parent = parent;
}

void sol_undo_record(sol_undo_tree* tree, sol_edit* edit) {
    if (!tree || !edit) return;
    
    sol_undo_node* node = create_node(tree, edit);
    if (!node) return;
    
    add_child(tree->current, node);
    tree->current = node;
    
    /* Free the original edit texts since we made copies */
    free(edit->old_text);
    free(edit->new_text);
    edit->old_text = NULL;
    edit->new_text = NULL;
}

sol_edit* sol_undo(sol_undo_tree* tree) {
    if (!tree || !tree->current || !tree->current->parent) {
        return NULL;
    }
    
    sol_edit* edit = &tree->current->edit;
    tree->current = tree->current->parent;
    
    return edit;
}

sol_edit* sol_redo(sol_undo_tree* tree) {
    if (!tree || !tree->current || tree->current->child_count == 0) {
        return NULL;
    }
    
    /* Follow active branch */
    int active = tree->current->active_child;
    if (active < 0 || active >= tree->current->child_count) {
        active = tree->current->child_count - 1;
    }
    
    tree->current = tree->current->children[active];
    return &tree->current->edit;
}

void sol_undo_begin_group(sol_undo_tree* tree) {
    if (!tree) return;
    
    sol_edit edit = {0};
    edit.type = SOL_EDIT_GROUP_START;
    edit.timestamp = sol_time_ms();
    
    sol_undo_node* node = create_node(tree, &edit);
    if (node) {
        add_child(tree->current, node);
        tree->current = node;
    }
    
    tree->group_depth++;
}

void sol_undo_end_group(sol_undo_tree* tree) {
    if (!tree || tree->group_depth <= 0) return;
    
    sol_edit edit = {0};
    edit.type = SOL_EDIT_GROUP_END;
    edit.timestamp = sol_time_ms();
    
    sol_undo_node* node = create_node(tree, &edit);
    if (node) {
        add_child(tree->current, node);
        tree->current = node;
    }
    
    tree->group_depth--;
}

bool sol_undo_can_undo(sol_undo_tree* tree) {
    return tree && tree->current && tree->current->parent != NULL;
}

bool sol_undo_can_redo(sol_undo_tree* tree) {
    return tree && tree->current && tree->current->child_count > 0;
}

int sol_undo_branch_count(sol_undo_tree* tree) {
    if (!tree || !tree->current) return 0;
    return tree->current->child_count;
}

void sol_undo_switch_branch(sol_undo_tree* tree, int branch_index) {
    if (!tree || !tree->current) return;
    
    if (branch_index >= 0 && branch_index < tree->current->child_count) {
        tree->current->active_child = branch_index;
    }
}
