// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ssh_config.c — see sol_ssh_config.h for the file format and the
   "never persist a password" contract.

   Hand-rolled minimal JSON parser, matching sol_settings.c's approach
   (no external dependency) rather than sharing code with it — the two
   schemas differ enough (array-of-objects here vs. a single top-level
   object there) that a shared parser would need to grow a generality
   neither file otherwise needs. */

#include "sol_ssh_config.h"
#include "sol_config.h"   /* sol_config_path() */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOL_SSH_CONFIG_FILENAME "ssh_connections.json"
#define SOL_SSH_DEFAULT_PORT    22u

/* ------------------------------------------------------------------ */
/* Minimal JSON parser (array-of-objects only — see file header)       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
} JP;

static void jp_skip_ws(JP *j)
{
    while (*j->p && isspace((unsigned char)*j->p)) ++j->p;
}

static bool jp_expect(JP *j, char c)
{
    jp_skip_ws(j);
    if (*j->p != c) return false;
    ++j->p;
    return true;
}

static bool jp_string(JP *j, char *buf, size_t bufsz)
{
    jp_skip_ws(j);
    if (*j->p != '"') return false;
    ++j->p;
    size_t i = 0;
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\') ++j->p;   /* skip escape */
        if (i + 1 < bufsz) buf[i++] = *j->p;
        ++j->p;
    }
    if (*j->p == '"') ++j->p;
    buf[i] = '\0';
    return true;
}

static long jp_int(JP *j)
{
    jp_skip_ws(j);
    const char *start = j->p;
    if (*j->p == '-' || *j->p == '+') ++j->p;
    while (isdigit((unsigned char)*j->p)) ++j->p;
    if (j->p == start) return -1;
    char tmp[32];
    size_t len = (size_t)(j->p - start);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    return strtol(tmp, NULL, 10);
}

static void jp_skip_value(JP *j)
{
    jp_skip_ws(j);
    if (!*j->p) return;
    if (*j->p == '"') {
        char tmp[512];
        jp_string(j, tmp, sizeof(tmp));
        return;
    }
    if (*j->p == '{' || *j->p == '[') {
        const char close = (*j->p == '{') ? '}' : ']';
        ++j->p;
        while (*j->p) {
            jp_skip_ws(j);
            if (*j->p == close) { ++j->p; return; }
            if (close == '}') {
                char key[128];
                jp_string(j, key, sizeof(key));
                jp_expect(j, ':');
            }
            jp_skip_value(j);
            jp_skip_ws(j);
            if (*j->p == ',') ++j->p;
        }
        return;
    }
    while (*j->p && !isspace((unsigned char)*j->p) &&
           *j->p != ',' && *j->p != '}' && *j->p != ']')
        ++j->p;
}

static SolSshAuthMethod jp_auth_method(const char *s)
{
    if (strcmp(s, "key") == 0) return SOL_SSH_AUTH_KEY;
    if (strcmp(s, "agent") == 0) return SOL_SSH_AUTH_AGENT;
    return SOL_SSH_AUTH_PASSWORD;
}

static const char *auth_method_str(SolSshAuthMethod m)
{
    switch (m) {
    case SOL_SSH_AUTH_KEY:   return "key";
    case SOL_SSH_AUTH_AGENT: return "agent";
    default:                 return "password";
    }
}

/* Parses one { ... } connection object at the cursor into conn.
   Unknown keys are skipped rather than rejected, so a future field
   added by a newer Sol version doesn't break an older one reading the
   same file. */
static bool jp_parse_connection(JP *j, SolSshConnection *conn)
{
    memset(conn, 0, sizeof(*conn));
    conn->port = (uint16_t)SOL_SSH_DEFAULT_PORT;

    if (!jp_expect(j, '{')) return false;
    while (*j->p) {
        jp_skip_ws(j);
        if (*j->p == '}') { ++j->p; break; }
        char key[64];
        if (!jp_string(j, key, sizeof(key))) return false;
        if (!jp_expect(j, ':')) return false;

        if (strcmp(key, "name") == 0) {
            jp_string(j, conn->name, sizeof(conn->name));
        } else if (strcmp(key, "host") == 0) {
            jp_string(j, conn->host, sizeof(conn->host));
        } else if (strcmp(key, "user") == 0) {
            jp_string(j, conn->user, sizeof(conn->user));
        } else if (strcmp(key, "key_path") == 0) {
            jp_string(j, conn->key_path, sizeof(conn->key_path));
        } else if (strcmp(key, "port") == 0) {
            long v = jp_int(j);
            if (v > 0 && v <= 65535) conn->port = (uint16_t)v;
        } else if (strcmp(key, "auth") == 0) {
            char auth_buf[16];
            jp_string(j, auth_buf, sizeof(auth_buf));
            conn->auth = jp_auth_method(auth_buf);
        } else {
            jp_skip_value(j);
        }
        jp_skip_ws(j);
        if (*j->p == ',') ++j->p;
    }
    /* A profile with no host is not usable — treat as a parse failure
       so the caller skips it rather than saving a blank entry back out
       on the next sol_ssh_config_upsert. */
    return conn->host[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool sol_ssh_config_load(SolSshConnectionList *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    char *path = sol_config_path(SOL_SSH_CONFIG_FILENAME);
    if (!path) return false;

    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp) return true;   /* no file yet — empty list is correct */

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    bool ok = false;
    if (size > 0 && size < 262144) {
        char *buf = (char *)malloc((size_t)size + 1);
        if (buf) {
            if ((long)fread(buf, 1, (size_t)size, fp) == size) {
                buf[size] = '\0';
                JP j = { .p = buf };
                jp_skip_ws(&j);
                if (*j.p == '[') {
                    ++j.p;
                    while (*j.p) {
                        jp_skip_ws(&j);
                        if (*j.p == ']') { ++j.p; break; }
                        if (out->count < SOL_SSH_CONNECTION_MAX) {
                            if (jp_parse_connection(&j, &out->items[out->count])) {
                                out->count++;
                            } else {
                                fprintf(stderr,
                                    "sol: skipping malformed entry in %s\n",
                                    SOL_SSH_CONFIG_FILENAME);
                            }
                        } else {
                            /* At capacity — still parse-and-discard so a
                               trailing comma / the closing ']' is consumed
                               correctly instead of leaving the cursor
                               stuck mid-array. */
                            SolSshConnection discard;
                            jp_parse_connection(&j, &discard);
                        }
                        jp_skip_ws(&j);
                        if (*j.p == ',') ++j.p;
                    }
                }
                ok = true;
            }
            free(buf);
        }
    }

    fclose(fp);
    return ok;
}

bool sol_ssh_config_save(const SolSshConnectionList *list)
{
    if (!list) return false;

    char *path = sol_config_path(SOL_SSH_CONFIG_FILENAME);
    if (!path) return false;

    FILE *fp = fopen(path, "wb");
    free(path);
    if (!fp) return false;

    fprintf(fp, "[\n");
    for (size_t i = 0u; i < list->count; ++i) {
        const SolSshConnection *c = &list->items[i];

        /* Escape quotes/backslashes for JSON safety — a host/user/path
           the user typed by hand could contain either. */
        char esc_name[sizeof(c->name) * 2];
        char esc_host[sizeof(c->host) * 2];
        char esc_user[sizeof(c->user) * 2];
        char esc_key[sizeof(c->key_path) * 2];
        const char *srcs[4]  = { c->name, c->host, c->user, c->key_path };
        char       *dsts[4]  = { esc_name, esc_host, esc_user, esc_key };
        for (int f = 0; f < 4; ++f) {
            const char *src = srcs[f];
            char       *dst = dsts[f];
            while (*src) {
                if (*src == '"' || *src == '\\') *dst++ = '\\';
                *dst++ = *src++;
            }
            *dst = '\0';
        }

        fprintf(fp,
            "  { \"name\": \"%s\", \"host\": \"%s\", \"port\": %u, "
            "\"user\": \"%s\", \"auth\": \"%s\", \"key_path\": \"%s\" }%s\n",
            esc_name, esc_host, (unsigned)c->port, esc_user,
            auth_method_str(c->auth), esc_key,
            (i + 1u < list->count) ? "," : "");
    }
    int n = fprintf(fp, "]\n");

    fclose(fp);
    return n > 0;
}

bool sol_ssh_config_upsert(const SolSshConnection *conn)
{
    if (!conn || conn->name[0] == '\0' || conn->host[0] == '\0') return false;

    SolSshConnectionList list;
    if (!sol_ssh_config_load(&list)) return false;

    for (size_t i = 0u; i < list.count; ++i) {
        if (strcmp(list.items[i].name, conn->name) == 0) {
            list.items[i] = *conn;
            return sol_ssh_config_save(&list);
        }
    }
    if (list.count >= SOL_SSH_CONNECTION_MAX) return false;
    list.items[list.count++] = *conn;
    return sol_ssh_config_save(&list);
}

bool sol_ssh_config_remove(const char *name)
{
    if (!name || name[0] == '\0') return false;

    SolSshConnectionList list;
    if (!sol_ssh_config_load(&list)) return false;

    for (size_t i = 0u; i < list.count; ++i) {
        if (strcmp(list.items[i].name, name) != 0) continue;
        for (size_t j = i; j + 1u < list.count; ++j) list.items[j] = list.items[j + 1u];
        list.count--;
        return sol_ssh_config_save(&list);
    }
    return false;
}
