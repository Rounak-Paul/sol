/*
 * Sol IDE - Glob Pattern Matching Implementation
 */

#include "sol.h"
#include <string.h>

bool sol_glob_match(const char* pattern, const char* str) {
    if (!pattern || !str) return false;
    
    const char* p = pattern;
    const char* s = str;
    const char* star_p = NULL;
    const char* star_s = NULL;
    
    while (*s) {
        if (*p == '*') {
            /* Remember position for backtracking */
            star_p = p++;
            star_s = s;
        } else if (*p == '?' || *p == *s) {
            /* Match single character */
            p++;
            s++;
        } else if (*p == '[') {
            /* Character class */
            bool negated = false;
            bool matched = false;
            
            p++;
            if (*p == '!' || *p == '^') {
                negated = true;
                p++;
            }
            
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    /* Range */
                    if (*s >= p[0] && *s <= p[2]) {
                        matched = true;
                    }
                    p += 3;
                } else {
                    if (*p == *s) {
                        matched = true;
                    }
                    p++;
                }
            }
            
            if (*p == ']') p++;
            
            if (matched == negated) {
                /* No match, try backtracking */
                if (star_p) {
                    p = star_p + 1;
                    s = ++star_s;
                } else {
                    return false;
                }
            } else {
                s++;
            }
        } else if (star_p) {
            /* Backtrack to * */
            p = star_p + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    
    /* Skip trailing *s */
    while (*p == '*') p++;
    
    return *p == '\0';
}
