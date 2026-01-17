/*
 * Sol IDE - UTF-8 Utilities Implementation
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Decode a UTF-8 character, return number of bytes consumed */
int sol_utf8_decode(const char* str, uint32_t* out) {
    if (!str || !out) return 0;
    
    const uint8_t* s = (const uint8_t*)str;
    uint8_t b0 = s[0];
    
    if (b0 == 0) {
        *out = 0;
        return 0;
    }
    
    if (b0 < 0x80) {
        /* ASCII */
        *out = b0;
        return 1;
    }
    
    if ((b0 & 0xE0) == 0xC0) {
        /* 2-byte sequence */
        if ((s[1] & 0xC0) != 0x80) {
            *out = 0xFFFD;  /* Replacement character */
            return 1;
        }
        *out = ((uint32_t)(b0 & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    
    if ((b0 & 0xF0) == 0xE0) {
        /* 3-byte sequence */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return 1;
        }
        *out = ((uint32_t)(b0 & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) |
               (s[2] & 0x3F);
        return 3;
    }
    
    if ((b0 & 0xF8) == 0xF0) {
        /* 4-byte sequence */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return 1;
        }
        *out = ((uint32_t)(b0 & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) |
               (s[3] & 0x3F);
        return 4;
    }
    
    /* Invalid UTF-8 */
    *out = 0xFFFD;
    return 1;
}

/* Encode a codepoint to UTF-8, return number of bytes written */
int sol_utf8_encode(uint32_t cp, char* out) {
    if (!out) return 0;
    
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    
    /* Invalid codepoint */
    out[0] = '?';
    return 1;
}

/* Count UTF-8 characters in string */
size_t sol_utf8_strlen(const char* str) {
    if (!str) return 0;
    
    size_t len = 0;
    while (*str) {
        uint32_t cp;
        int bytes = sol_utf8_decode(str, &cp);
        if (bytes == 0) break;
        str += bytes;
        len++;
    }
    
    return len;
}

/* String utilities */
char* sol_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

char* sol_strndup(const char* str, size_t len) {
    if (!str) return NULL;
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    return copy;
}

bool sol_str_starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return false;
    size_t prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}

bool sol_str_ends_with(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return false;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

char* sol_str_trim(char* str) {
    if (!str) return NULL;
    
    /* Trim leading whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }
    
    if (*str == '\0') return str;
    
    /* Trim trailing whitespace */
    char* end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }
    end[1] = '\0';
    
    return str;
}
