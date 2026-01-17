/*
 * Sol IDE - Fuzzy Matching Implementation
 * 
 * Used for command palette and file picker.
 * Returns match score (higher is better) and match positions.
 */

#include "sol.h"
#include <string.h>
#include <ctype.h>

#define SCORE_GAP_START         -3
#define SCORE_GAP_EXTENSION     -1
#define SCORE_MATCH_CONSECUTIVE 5
#define SCORE_MATCH_WORD_START  10
#define SCORE_MATCH_CAMEL       8
#define SCORE_MATCH_SLASH       15

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_word_start(const char* str, int pos) {
    if (pos == 0) return true;
    
    char prev = str[pos - 1];
    char curr = str[pos];
    
    /* After separator */
    if (!is_word_char(prev) && is_word_char(curr)) {
        return true;
    }
    
    /* CamelCase boundary */
    if (islower((unsigned char)prev) && isupper((unsigned char)curr)) {
        return true;
    }
    
    return false;
}

int sol_fuzzy_match(const char* pattern, const char* str, int* out_score) {
    if (!pattern || !str) {
        if (out_score) *out_score = 0;
        return 0;
    }
    
    if (!*pattern) {
        if (out_score) *out_score = 0;
        return 1;  /* Empty pattern matches everything */
    }
    
    int pattern_len = (int)strlen(pattern);
    int str_len = (int)strlen(str);
    
    if (pattern_len > str_len) {
        if (out_score) *out_score = 0;
        return 0;
    }
    
    /* Simple scoring algorithm */
    int score = 0;
    int pattern_idx = 0;
    int prev_match = -1;
    bool in_gap = false;
    
    for (int i = 0; i < str_len && pattern_idx < pattern_len; i++) {
        char pc = tolower((unsigned char)pattern[pattern_idx]);
        char sc = tolower((unsigned char)str[i]);
        
        if (pc == sc) {
            /* Match */
            if (prev_match == i - 1) {
                score += SCORE_MATCH_CONSECUTIVE;
            }
            
            if (is_word_start(str, i)) {
                if (i > 0 && (str[i-1] == '/' || str[i-1] == '\\')) {
                    score += SCORE_MATCH_SLASH;
                } else if (isupper((unsigned char)str[i]) && i > 0 && 
                           islower((unsigned char)str[i-1])) {
                    score += SCORE_MATCH_CAMEL;
                } else {
                    score += SCORE_MATCH_WORD_START;
                }
            }
            
            pattern_idx++;
            prev_match = i;
            in_gap = false;
        } else {
            /* Gap */
            if (prev_match >= 0) {
                if (!in_gap) {
                    score += SCORE_GAP_START;
                    in_gap = true;
                } else {
                    score += SCORE_GAP_EXTENSION;
                }
            }
        }
    }
    
    if (pattern_idx < pattern_len) {
        /* Not all characters matched */
        if (out_score) *out_score = 0;
        return 0;
    }
    
    /* Bonus for matching at start */
    if (str_len > 0 && tolower((unsigned char)pattern[0]) == 
                       tolower((unsigned char)str[0])) {
        score += 15;
    }
    
    /* Bonus for exact match */
    if (pattern_len == str_len) {
        score += 20;
    }
    
    /* Normalize by pattern length */
    score += pattern_len * 2;
    
    if (out_score) *out_score = score;
    return 1;
}
