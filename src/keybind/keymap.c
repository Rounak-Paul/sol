/*
 * Sol IDE - Keybinding System Implementation
 * 
 * Supports vim-style key sequences (e.g., "ctrl+k ctrl+c")
 * and modifiers (ctrl, alt, shift, super/cmd).
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEYBINDS 512

typedef struct {
    sol_keysequence sequence;
    char* command;
    char* when;             /* Context condition */
} keymap_entry;

struct sol_keymap {
    keymap_entry entries[MAX_KEYBINDS];
    int count;
};

/* Parse a single key chord like "ctrl+k" */
static sol_keychord parse_chord(const char* str, const char** end) {
    sol_keychord chord = {0};
    
    while (*str && *str != ' ') {
        /* Skip whitespace */
        while (*str == ' ') str++;
        if (!*str || *str == ' ') break;
        
        /* Check for modifiers */
        if (strncmp(str, "ctrl+", 5) == 0 || strncmp(str, "Ctrl+", 5) == 0 ||
            strncmp(str, "C-", 2) == 0) {
            chord.mods |= SOL_MOD_CTRL;
            str += (str[1] == '-') ? 2 : 5;
            continue;
        }
        if (strncmp(str, "alt+", 4) == 0 || strncmp(str, "Alt+", 4) == 0 ||
            strncmp(str, "M-", 2) == 0) {
            chord.mods |= SOL_MOD_ALT;
            str += (str[1] == '-') ? 2 : 4;
            continue;
        }
        if (strncmp(str, "shift+", 6) == 0 || strncmp(str, "Shift+", 6) == 0 ||
            strncmp(str, "S-", 2) == 0) {
            chord.mods |= SOL_MOD_SHIFT;
            str += (str[1] == '-') ? 2 : 6;
            continue;
        }
        if (strncmp(str, "super+", 6) == 0 || strncmp(str, "Super+", 6) == 0 ||
            strncmp(str, "cmd+", 4) == 0 || strncmp(str, "Cmd+", 4) == 0) {
            chord.mods |= SOL_MOD_SUPER;
            str += (str[1] == 'm' || str[1] == 'M') ? 4 : 6;
            continue;
        }
        
        /* Parse key name */
        if (strncmp(str, "up", 2) == 0) { chord.key = SOL_KEY_UP; str += 2; }
        else if (strncmp(str, "down", 4) == 0) { chord.key = SOL_KEY_DOWN; str += 4; }
        else if (strncmp(str, "left", 4) == 0) { chord.key = SOL_KEY_LEFT; str += 4; }
        else if (strncmp(str, "right", 5) == 0) { chord.key = SOL_KEY_RIGHT; str += 5; }
        else if (strncmp(str, "enter", 5) == 0 || strncmp(str, "return", 6) == 0) { 
            chord.key = SOL_KEY_ENTER; 
            str += (*str == 'e') ? 5 : 6; 
        }
        else if (strncmp(str, "tab", 3) == 0) { chord.key = SOL_KEY_TAB; str += 3; }
        else if (strncmp(str, "escape", 6) == 0 || strncmp(str, "esc", 3) == 0) { 
            chord.key = SOL_KEY_ESCAPE; 
            str += (*str == 'e' && str[1] == 's' && str[2] == 'c' && str[3] != 'a') ? 3 : 6; 
        }
        else if (strncmp(str, "backspace", 9) == 0) { chord.key = SOL_KEY_BACKSPACE; str += 9; }
        else if (strncmp(str, "delete", 6) == 0) { chord.key = SOL_KEY_DELETE; str += 6; }
        else if (strncmp(str, "home", 4) == 0) { chord.key = SOL_KEY_HOME; str += 4; }
        else if (strncmp(str, "end", 3) == 0) { chord.key = SOL_KEY_END; str += 3; }
        else if (strncmp(str, "pageup", 6) == 0) { chord.key = SOL_KEY_PAGE_UP; str += 6; }
        else if (strncmp(str, "pagedown", 8) == 0) { chord.key = SOL_KEY_PAGE_DOWN; str += 8; }
        else if (strncmp(str, "insert", 6) == 0) { chord.key = SOL_KEY_INSERT; str += 6; }
        else if (strncmp(str, "space", 5) == 0) { chord.key = ' '; str += 5; }
        else if (str[0] == 'f' && isdigit((unsigned char)str[1])) {
            int fn = atoi(str + 1);
            if (fn >= 1 && fn <= 12) {
                chord.key = SOL_KEY_F1 + fn - 1;
            }
            str++;
            while (isdigit((unsigned char)*str)) str++;
        }
        else if (isalnum((unsigned char)*str)) {
            chord.key = tolower((unsigned char)*str);
            str++;
        }
        else {
            /* Unknown, skip */
            str++;
        }
        
        break;  /* After parsing key, we're done with this chord */
    }
    
    if (end) *end = str;
    return chord;
}

sol_keysequence sol_keymap_parse(const char* keys) {
    sol_keysequence seq = {0};
    
    if (!keys) return seq;
    
    const char* p = keys;
    
    while (*p && seq.length < SOL_MAX_KEY_SEQUENCE) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (!*p) break;
        
        seq.chords[seq.length++] = parse_chord(p, &p);
        
        /* Skip whitespace between chords */
        while (*p == ' ') p++;
    }
    
    return seq;
}

sol_keymap* sol_keymap_create(void) {
    sol_keymap* km = (sol_keymap*)calloc(1, sizeof(sol_keymap));
    return km;
}

void sol_keymap_destroy(sol_keymap* km) {
    if (!km) return;
    
    for (int i = 0; i < km->count; i++) {
        free(km->entries[i].command);
        free(km->entries[i].when);
    }
    
    free(km);
}

void sol_keymap_bind(sol_keymap* km, const char* keys, const char* command, const char* when) {
    if (!km || !keys || !command) return;
    if (km->count >= MAX_KEYBINDS) return;
    
    keymap_entry* entry = &km->entries[km->count++];
    entry->sequence = sol_keymap_parse(keys);
    entry->command = sol_strdup(command);
    entry->when = when ? sol_strdup(when) : NULL;
}

void sol_keymap_unbind(sol_keymap* km, const char* keys) {
    if (!km || !keys) return;
    
    sol_keysequence seq = sol_keymap_parse(keys);
    
    for (int i = 0; i < km->count; i++) {
        /* Compare sequences */
        keymap_entry* entry = &km->entries[i];
        if (entry->sequence.length != seq.length) continue;
        
        bool match = true;
        for (int j = 0; j < seq.length; j++) {
            if (entry->sequence.chords[j].key != seq.chords[j].key ||
                entry->sequence.chords[j].mods != seq.chords[j].mods) {
                match = false;
                break;
            }
        }
        
        if (match) {
            /* Remove this entry */
            free(entry->command);
            free(entry->when);
            
            /* Shift remaining entries */
            for (int j = i; j < km->count - 1; j++) {
                km->entries[j] = km->entries[j + 1];
            }
            km->count--;
            return;
        }
    }
}

/* Check if context condition matches */
static bool context_matches(const char* when, const char* context) {
    if (!when) return true;  /* No condition = always matches */
    if (!context) return false;
    
    /* Simple contains check for now */
    /* TODO: Proper expression parser */
    return strstr(context, when) != NULL;
}

/* Check if two sequences match up to a length */
static bool sequence_matches(sol_keysequence* a, sol_keysequence* b, int len) {
    if (len > a->length || len > b->length) return false;
    
    for (int i = 0; i < len; i++) {
        if (a->chords[i].key != b->chords[i].key ||
            a->chords[i].mods != b->chords[i].mods) {
            return false;
        }
    }
    
    return true;
}

const char* sol_keymap_lookup(sol_keymap* km, sol_keysequence* seq, const char* context) {
    if (!km || !seq || seq->length == 0) return NULL;
    
    /* Look for exact match */
    for (int i = 0; i < km->count; i++) {
        keymap_entry* entry = &km->entries[i];
        
        if (entry->sequence.length == seq->length &&
            sequence_matches(&entry->sequence, seq, seq->length) &&
            context_matches(entry->when, context)) {
            return entry->command;
        }
    }
    
    return NULL;
}

/* Check if current sequence could be a prefix of any binding */
bool sol_keymap_has_prefix(sol_keymap* km, sol_keysequence* seq) {
    if (!km || !seq || seq->length == 0) return false;
    
    for (int i = 0; i < km->count; i++) {
        keymap_entry* entry = &km->entries[i];
        
        if (entry->sequence.length > seq->length &&
            sequence_matches(&entry->sequence, seq, seq->length)) {
            return true;
        }
    }
    
    return false;
}
