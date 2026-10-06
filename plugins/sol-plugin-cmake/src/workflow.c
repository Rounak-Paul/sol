// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* CMake project workflow. CMake's file API is authoritative after the first
 * configure; source declarations provide an immediate list before that. */
#include "workflow.h"

#include <causality.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_event.h"
#include "sol_platform.h"
#include "sol_ui_system.h"

#define CMAKE_TARGET_CAP 256
#define CMAKE_PATH_CAP 4096

typedef struct CmakeTarget {
    char name[128];
    char artifact[CMAKE_PATH_CAP];
    bool executable;
} CmakeTarget;

typedef struct CmakeWorkflow {
    SolPluginCtx *ctx;
    SolPluginToolbarToken toolbar;
    SolSubscriptionToken root_sub;
    SolSubscriptionToken fs_sub;
    char root[CMAKE_PATH_CAP];
    char build_dir[CMAKE_PATH_CAP];
    char configuration[64];
    CmakeTarget targets[CMAKE_TARGET_CAP];
    char target_labels[CMAKE_TARGET_CAP][32];
    const char *build_options[CMAKE_TARGET_CAP + 1];
    const char *run_options[CMAKE_TARGET_CAP];
    int run_indices[CMAKE_TARGET_CAP];
    int target_count;
    int run_count;
    int selected_build;
    int selected_run;
    bool detected;
    bool has_codemodel;
    Ca_Tooltip *build_tooltip;
    Ca_Tooltip *run_tooltip;
} CmakeWorkflow;

static void destroy_workflow(void *service, void *user_data)
{
    (void)user_data;
    free(service);
}

static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n >= 0 && (size_t)n < cap;
}

static bool file_exists(const char *path)
{
    SolPathInfo info;
    return sol_platform_get_path_info(path, &info) && !info.is_directory;
}

static bool cache_matches_root(const char *cache_path, const char *root)
{
    FILE *file = fopen(cache_path, "rb");
    if (!file) return false;
    char line[CMAKE_PATH_CAP + 80];
    bool matches = false;
    while (fgets(line, sizeof(line), file)) {
        const char *key = "CMAKE_HOME_DIRECTORY:INTERNAL=";
        if (strncmp(line, key, strlen(key)) != 0) continue;
        char *value = line + strlen(key);
        value[strcspn(value, "\r\n")] = '\0';
        matches = strcmp(value, root) == 0;
        break;
    }
    fclose(file);
    return matches;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || length > 32 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return NULL;
    }
    char *data = malloc((size_t)length + 1);
    if (!data) { fclose(file); return NULL; }
    size_t got = fread(data, 1, (size_t)length, file);
    fclose(file);
    data[got] = '\0';
    return data;
}

static bool valid_target_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= sizeof(((CmakeTarget *)0)->name)) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != '+') return false;
    return true;
}

/* Keep both ends of long names visible in the fixed-width target selector. */
static void target_label(char out[32], const char *name)
{
    size_t len = strlen(name);
    if (len <= 17u) {
        snprintf(out, 32, "%s", name);
        return;
    }
    snprintf(out, 32, "%.7s...%s", name, name + len - 7u);
}

static void add_target(CmakeWorkflow *w, const char *name, bool executable,
                       const char *artifact)
{
    if (!valid_target_name(name)) return;
    for (int i = 0; i < w->target_count; ++i) {
        CmakeTarget *target = &w->targets[i];
        if (strcmp(target->name, name) != 0) continue;
        target->executable = executable;
        if (artifact && artifact[0])
            snprintf(target->artifact, sizeof(target->artifact), "%s", artifact);
        return;
    }
    if (w->target_count >= CMAKE_TARGET_CAP) return;
    CmakeTarget *target = &w->targets[w->target_count++];
    memset(target, 0, sizeof(*target));
    snprintf(target->name, sizeof(target->name), "%s", name);
    target->executable = executable;
    if (artifact) snprintf(target->artifact, sizeof(target->artifact), "%s", artifact);
}

/* Locate a JSON string field within one object. CMake-generated paths may
 * contain escaped separators; this handles the escapes JSON uses there. */
static bool json_string(const char *begin, const char *end, const char *key,
                        char *out, size_t cap)
{
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *at = begin;
    while ((at = strstr(at, pattern)) && at < end) {
        at += strlen(pattern);
        while (at < end && isspace((unsigned char)*at)) ++at;
        if (at >= end || *at++ != ':') continue;
        while (at < end && isspace((unsigned char)*at)) ++at;
        if (at >= end || *at++ != '"') continue;
        size_t n = 0;
        while (at < end && *at != '"') {
            char ch = *at++;
            if (ch == '\\' && at < end) ch = *at++;
            if (n + 1 >= cap) return false;
            out[n++] = ch;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

static void scan_reply(CmakeWorkflow *w)
{
    char reply[CMAKE_PATH_CAP];
    if (!join_path(reply, sizeof(reply), w->build_dir, ".cmake/api/v1/reply")) return;
    SolDirectoryIter iter;
    SolDirectoryEntry entry;
    if (!sol_platform_dir_open(&iter, reply)) return;
    char index_name[256] = "";
    while (sol_platform_dir_next(&iter, &entry)) {
        if (strncmp(entry.name, "index-", 6) == 0 &&
            strstr(entry.name, ".json") && strcmp(entry.name, index_name) > 0)
            snprintf(index_name, sizeof(index_name), "%s", entry.name);
    }
    sol_platform_dir_close(&iter);
    if (!index_name[0]) return;
    char path[CMAKE_PATH_CAP];
    if (!join_path(path, sizeof(path), reply, index_name)) return;
    char *index = read_file(path);
    if (!index) return;
    const char *query = strstr(index, "\"client-sol\"");
    const char *model_key = query ? strstr(query, "\"codemodel-v2\"") : NULL;
    char codemodel[256] = "";
    if (model_key) {
        const char *end = strchr(model_key, '}');
        if (end) json_string(model_key, end, "jsonFile", codemodel, sizeof(codemodel));
    }
    free(index);
    if (strncmp(codemodel, "codemodel-v2-", 13) != 0 || strchr(codemodel, '/')) return;
    if (!join_path(path, sizeof(path), reply, codemodel)) return;
    char *model = read_file(path);
    if (!model) return;
    const char *targets = strstr(model, "\"targets\"");
    const char *targets_end = targets ? strchr(targets, ']') : NULL;
    const char *configs = strstr(model, "\"configurations\"");
    const char *first_config = configs ? strchr(configs, '{') : NULL;
    if (first_config && targets && first_config < targets)
        json_string(first_config, targets, "name", w->configuration,
                    sizeof(w->configuration));
    const char *at = targets;
    while (at && targets_end && (at = strstr(at, "\"jsonFile\"")) && at < targets_end) {
        const char *obj = at;
        while (obj > model && *obj != '{') --obj;
        const char *end = strchr(at, '}');
        if (!end) break;
        char name[128], filename[256];
        if (json_string(obj, end, "name", name, sizeof(name)) &&
            json_string(obj, end, "jsonFile", filename, sizeof(filename)) &&
            strncmp(filename, "target-", 7) == 0 &&
            !strchr(filename, '/') && !strchr(filename, '\\') &&
            join_path(path, sizeof(path), reply, filename)) {
            char *target_json = read_file(path);
            if (target_json) {
                char type[64] = "", artifact[CMAKE_PATH_CAP] = "";
                const char *target_end = target_json + strlen(target_json);
                json_string(target_json, target_end, "type", type, sizeof(type));
                const char *artifacts = strstr(target_json, "\"artifacts\"");
                if (artifacts) {
                    const char *art_end = strchr(artifacts, ']');
                    if (art_end) json_string(artifacts, art_end, "path", artifact, sizeof(artifact));
                }
                add_target(w, name, strcmp(type, "EXECUTABLE") == 0, artifact);
                free(target_json);
            }
        }
        at = end + 1;
    }
    free(model);
}

/* Read simple target declarations immediately. The generated codemodel above
 * replaces their guessed type/artifact when CMake has configured the tree. */
static void scan_source(CmakeWorkflow *w, const char *directory, int depth)
{
    if (depth > 8) return;
    char path[CMAKE_PATH_CAP];
    if (!join_path(path, sizeof(path), directory, "CMakeLists.txt")) return;
    char *source = read_file(path);
    if (!source) return;
    for (char *at = source; *at;) {
        if (*at == '#') { while (*at && *at != '\n') ++at; continue; }
        if (!isalpha((unsigned char)*at) && *at != '_') { ++at; continue; }
        char *start = at;
        while (isalnum((unsigned char)*at) || *at == '_') ++at;
        size_t len = (size_t)(at - start);
        while (isspace((unsigned char)*at)) ++at;
        if (*at != '(') continue;
        ++at;
        while (isspace((unsigned char)*at)) ++at;
        char argument[256]; size_t n = 0;
        bool quoted = *at == '"';
        if (quoted) ++at;
        while (*at && n + 1 < sizeof(argument) &&
               (quoted ? *at != '"' : !isspace((unsigned char)*at) && *at != ')'))
            argument[n++] = *at++;
        argument[n] = '\0';
        if (len == 14 && strncmp(start, "add_executable", len) == 0) {
            char artifact[CMAKE_PATH_CAP];
            const char *relative = directory + strlen(w->root);
            while (*relative == '/' || *relative == '\\') ++relative;
            if (*relative) {
                if (!join_path(artifact, sizeof(artifact), relative, argument)) continue;
            } else {
                snprintf(artifact, sizeof(artifact), "%s", argument);
            }
#ifdef _WIN32
            size_t used = strlen(artifact);
            if (used + 4 < sizeof(artifact)) strcat(artifact, ".exe");
#endif
            add_target(w, argument, true, artifact);
        }
        else if ((len == 11 && strncmp(start, "add_library", len) == 0) ||
                 (len == 17 && strncmp(start, "add_custom_target", len) == 0))
            add_target(w, argument, false, NULL);
        else if (len == 16 && strncmp(start, "add_subdirectory", len) == 0 &&
                 argument[0] && argument[0] != '$' && argument[0] != '/' &&
                 !strstr(argument, "..")) {
            char subdir[CMAKE_PATH_CAP];
            if (join_path(subdir, sizeof(subdir), directory, argument))
                scan_source(w, subdir, depth + 1);
        }
    }
    free(source);
}

static void query_file_api(CmakeWorkflow *w)
{
    char query[CMAKE_PATH_CAP];
    if (!join_path(query, sizeof(query), w->build_dir,
                   ".cmake/api/v1/query/client-sol")) return;
    if (!sol_platform_mkdir_p(query)) return;
    char path[CMAKE_PATH_CAP];
    if (!join_path(path, sizeof(path), query, "codemodel-v2")) return;
    FILE *file = fopen(path, "ab");
    if (file) fclose(file);
}

static void rebuild_options(CmakeWorkflow *w, const char *build_name,
                            const char *run_name)
{
    w->build_options[0] = "All targets";
    w->selected_build = 0;
    w->selected_run = 0;
    w->run_count = 0;
    for (int i = 0; i < w->target_count; ++i) {
        CmakeTarget *target = &w->targets[i];
        target_label(w->target_labels[i], target->name);
        w->build_options[i + 1] = w->target_labels[i];
        if (build_name && strcmp(target->name, build_name) == 0)
            w->selected_build = i + 1;
        if (!target->executable) continue;
        w->run_indices[w->run_count] = i;
        w->run_options[w->run_count] = w->target_labels[i];
        if (run_name && strcmp(target->name, run_name) == 0)
            w->selected_run = w->run_count;
        ++w->run_count;
    }
}

static void rescan(CmakeWorkflow *w, const char *root)
{
    char root_copy[CMAKE_PATH_CAP];
    if (root && strlen(root) < sizeof(root_copy)) {
        snprintf(root_copy, sizeof(root_copy), "%s", root);
        root = root_copy;
    }
    char old_build[128] = "", old_run[128] = "";
    if (w->selected_build > 0 && w->selected_build <= w->target_count)
        snprintf(old_build, sizeof(old_build), "%s", w->targets[w->selected_build - 1].name);
    if (w->selected_run >= 0 && w->selected_run < w->run_count)
        snprintf(old_run, sizeof(old_run), "%s",
                 w->targets[w->run_indices[w->selected_run]].name);
    w->detected = false;
    w->has_codemodel = false;
    w->target_count = w->run_count = 0;
    w->configuration[0] = '\0';
    if (!root || strlen(root) >= sizeof(w->root)) { w->root[0] = '\0'; goto done; }
    snprintf(w->root, sizeof(w->root), "%s", root);
    char path[CMAKE_PATH_CAP];
    if (!join_path(path, sizeof(path), root, "CMakeLists.txt") || !file_exists(path)) goto done;
    w->detected = true;
    const char *candidates[] = {
        "build", "build-debug", "build-release", "cmake-build-debug",
        "cmake-build-release", "out/build", "",
    };
    if (!join_path(w->build_dir, sizeof(w->build_dir), root, "build")) goto done;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (candidates[i][0]) {
            if (!join_path(path, sizeof(path), root, candidates[i])) continue;
        } else {
            snprintf(path, sizeof(path), "%s", root);
        }
        char cache[CMAKE_PATH_CAP];
        if (join_path(cache, sizeof(cache), path, "CMakeCache.txt") &&
            cache_matches_root(cache, root)) {
            snprintf(w->build_dir, sizeof(w->build_dir), "%s", path);
            break;
        }
    }
    SolPathInfo build_info;
    if (sol_platform_get_path_info(w->build_dir, &build_info) && build_info.is_directory)
        query_file_api(w);
    scan_reply(w);
    w->has_codemodel = w->target_count > 0;
    if (!w->target_count) scan_source(w, root, 0);
done:
    rebuild_options(w, old_build, old_run);
    sol_plugin_notify_toolbar(w->ctx);
}

static bool root_changed(const SolEvent *event, void *data)
{
    CmakeWorkflow *w = data;
    const SolFileTreeRootPayload *payload = event ? event->payload : NULL;
    rescan(w, payload ? payload->path : NULL);
    return false;
}

static bool fs_changed(const SolEvent *event, void *data)
{
    CmakeWorkflow *w = data;
    const SolFsChangedPayload *payload = event ? event->payload : NULL;
    if (!w->root[0] || !payload) return false;
    for (size_t i = 0; i < payload->count; ++i) {
        const char *path = payload->events[i].path;
        if (strstr(path, "CMakeLists.txt") ||
            (strstr(path, "reply") && strstr(path, "index-"))) {
            rescan(w, w->root);
            break;
        }
    }
    return false;
}

static bool shell_quote(const char *value, char *out, size_t cap)
{
    size_t n = 0;
#ifdef _WIN32
    if (n + 1 >= cap) return false;
    out[n++] = '"';
    for (const char *p = value; *p; ++p) {
        if (*p == '"' || *p == '\r' || *p == '\n' || n + 2 >= cap) return false;
        out[n++] = *p;
    }
    out[n++] = '"';
#else
    if (n + 1 >= cap) return false;
    out[n++] = '\'';
    for (const char *p = value; *p; ++p) {
        if (n + 5 >= cap) return false;
        if (*p == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
        else out[n++] = *p;
    }
    out[n++] = '\'';
#endif
    out[n] = '\0';
    return true;
}

typedef enum CmakeAction {
    CMAKE_ACTION_CONFIGURE,
    CMAKE_ACTION_BUILD,
    CMAKE_ACTION_RUN,
} CmakeAction;

static void execute(CmakeWorkflow *w, CmakeAction action)
{
    if (!w->detected) return;
    query_file_api(w);
    CmakeTarget *target = NULL;
    if (action == CMAKE_ACTION_RUN) {
        if (w->selected_run < 0 || w->selected_run >= w->run_count) return;
        target = &w->targets[w->run_indices[w->selected_run]];
    } else if (action == CMAKE_ACTION_BUILD &&
               w->selected_build > 0 && w->selected_build <= w->target_count) {
        target = &w->targets[w->selected_build - 1];
    }
    char source_q[2 * CMAKE_PATH_CAP], build_q[2 * CMAKE_PATH_CAP];
    char target_q[512] = "", exe_q[2 * CMAKE_PATH_CAP] = "", config_q[160] = "";
    if (!shell_quote(w->root, source_q, sizeof(source_q)) ||
        !shell_quote(w->build_dir, build_q, sizeof(build_q))) return;
    if (target && !shell_quote(target->name, target_q, sizeof(target_q))) return;
    if (w->configuration[0] &&
        !shell_quote(w->configuration, config_q, sizeof(config_q))) return;
    char command[5 * CMAKE_PATH_CAP];
    char cache[CMAKE_PATH_CAP];
    bool configured = join_path(cache, sizeof(cache), w->build_dir, "CMakeCache.txt") &&
                      cache_matches_root(cache, w->root) && w->has_codemodel;
    int n = 0;
    if (action == CMAKE_ACTION_CONFIGURE || !configured) {
        n = snprintf(command, sizeof(command), "cmake -S %s -B %s", source_q, build_q);
        if (n < 0 || (size_t)n >= sizeof(command)) return;
    }
    if (action != CMAKE_ACTION_CONFIGURE) {
        int appended = snprintf(command + n, sizeof(command) - (size_t)n,
                                "%scmake --build %s%s%s%s%s",
                                n ? " && " : "", build_q, target ? " --target " : "",
                                target ? target_q : "", config_q[0] ? " --config " : "", config_q);
        if (appended < 0) return;
        n += appended;
    }
    if (n < 0 || (size_t)n >= sizeof(command)) return;
    if (action == CMAKE_ACTION_RUN && target) {
        char executable[CMAKE_PATH_CAP];
        const char *artifact = target->artifact[0] ? target->artifact : target->name;
        if (artifact[0] == '/' || (isalpha((unsigned char)artifact[0]) && artifact[1] == ':'))
            snprintf(executable, sizeof(executable), "%s", artifact);
        else if (!join_path(executable, sizeof(executable), w->build_dir, artifact)) return;
        if (!shell_quote(executable, exe_q, sizeof(exe_q))) return;
        snprintf(command + n, sizeof(command) - (size_t)n, " && %s", exe_q);
    }
    (void)sol_plugin_run_in_terminal(w->ctx, w->root, command);
}

static void build_clicked(Ca_Button *button, void *data)
{
    (void)button; execute(data, CMAKE_ACTION_BUILD);
}

static void run_clicked(Ca_Button *button, void *data)
{
    (void)button; execute(data, CMAKE_ACTION_RUN);
}

static void configure_clicked(Ca_Button *button, void *data)
{
    (void)button; execute(data, CMAKE_ACTION_CONFIGURE);
}

static void build_selected(Ca_Select *select, void *data)
{
    CmakeWorkflow *w = data;
    w->selected_build = ca_select_get(select);
    if (w->build_tooltip)
        ca_tooltip_set_text(w->build_tooltip,
            w->selected_build > 0 && w->selected_build <= w->target_count
                ? w->targets[w->selected_build - 1].name : "All targets");
}

static void run_selected(Ca_Select *select, void *data)
{
    CmakeWorkflow *w = data;
    w->selected_run = ca_select_get(select);
    if (w->run_tooltip)
        ca_tooltip_set_text(w->run_tooltip,
            w->selected_run >= 0 && w->selected_run < w->run_count
                ? w->targets[w->run_indices[w->selected_run]].name
                : "No executable target");
}

static void render_toolbar(void *data)
{
    CmakeWorkflow *w = data;
    if (!w->detected) return;
    ca_text(&(Ca_TextDesc){ .text = "CMake", .style = "toolbar-label" });
    Ca_Button *configure = ca_btn_begin(&(Ca_BtnDesc){
        .style = "toolbar-icon", .direction = CA_HORIZONTAL,
        .on_click = configure_clicked, .click_data = w, .skip_keyboard_focus = true,
    });
    ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_FA_GEAR, .style = "toolbar-icon-glyph" });
    ca_btn_end();
    ca_tooltip_for_widget(configure, &(Ca_TooltipDesc){ .text = "Configure CMake project" });
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "toolbar-cmake-group" });
    Ca_Select *build_target = ca_select(&(Ca_SelectDesc){
        .id = "cmake-build-target", .style = "toolbar-target-select",
        .options = w->build_options, .option_count = w->target_count + 1,
        .width = 160.0f,
        .selected = w->selected_build, .on_change = build_selected, .change_data = w,
    });
    w->build_tooltip = ca_tooltip_for_widget(build_target, &(Ca_TooltipDesc){
        .text = w->selected_build > 0 && w->selected_build <= w->target_count
            ? w->targets[w->selected_build - 1].name : "All targets",
    });
    Ca_Button *build = ca_btn_begin(&(Ca_BtnDesc){
        .style = "toolbar-icon", .direction = CA_HORIZONTAL,
        .on_click = build_clicked, .click_data = w, .skip_keyboard_focus = true,
    });
    ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_FA_HAMMER, .style = "toolbar-icon-glyph" });
    ca_btn_end();
    ca_tooltip_for_widget(build, &(Ca_TooltipDesc){ .text = "Build selected target" });
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "toolbar-cmake-group" });
    static const char *const no_executable[] = { "No executable" };
    Ca_Select *run_target = ca_select(&(Ca_SelectDesc){
        .id = "cmake-run-target", .style = "toolbar-target-select",
        .options = w->run_count ? w->run_options : no_executable,
        .option_count = w->run_count ? w->run_count : 1,
        .width = 160.0f, .selected = w->selected_run,
        .on_change = run_selected, .change_data = w,
        .disabled = w->run_count == 0,
    });
    w->run_tooltip = ca_tooltip_for_widget(run_target, &(Ca_TooltipDesc){
        .text = w->run_count && w->selected_run >= 0 && w->selected_run < w->run_count
            ? w->targets[w->run_indices[w->selected_run]].name : "No executable target",
    });
    Ca_Button *run = ca_btn_begin(&(Ca_BtnDesc){
        .style = "toolbar-icon", .direction = CA_HORIZONTAL,
        .on_click = run_clicked, .click_data = w, .skip_keyboard_focus = true,
        .disabled = w->run_count == 0,
    });
    ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_FA_PLAY, .style = "toolbar-icon-glyph" });
    ca_btn_end();
    ca_tooltip_for_widget(run, &(Ca_TooltipDesc){
        .text = w->run_count ? "Build and run CMake executable" : "No executable target found",
    });
    ca_div_end();
}

static bool command(const char *action, const SolInputEvent *event, void *data)
{
    (void)event;
    CmakeWorkflow *w = data;
    if (strcmp(action, "cmake.configure") == 0) { execute(w, CMAKE_ACTION_CONFIGURE); return true; }
    if (strcmp(action, "cmake.build") == 0) { execute(w, CMAKE_ACTION_BUILD); return true; }
    if (strcmp(action, "cmake.run") == 0) { execute(w, CMAKE_ACTION_RUN); return true; }
    return false;
}

bool cmake_workflow_load(SolPluginCtx *ctx)
{
    CmakeWorkflow *w = calloc(1, sizeof(*w));
    if (!w) return false;
    w->ctx = ctx;
    if (!sol_plugin_register_service(ctx, "cmake.workflow.state", 1u,
                                     w, destroy_workflow, NULL)) {
        free(w);
        return false;
    }
    w->toolbar = sol_plugin_register_toolbar(ctx, &(SolPluginToolbarDesc){
        .id = "cmake.workflow", .render = render_toolbar, .user_data = w, .order = 100,
    });
    if (!w->toolbar) return false;
    w->root_sub = sol_plugin_subscribe(ctx, SOL_EVENT_FILE_TREE_ROOT, root_changed, w);
    w->fs_sub = sol_plugin_subscribe(ctx, SOL_EVENT_FS_CHANGED, fs_changed, w);
    const SolPluginCommandDesc commands[] = {
        { .action = "cmake.configure", .label = "Configure CMake project", .callback = command, .user_data = w },
        { .action = "cmake.build", .label = "Build CMake target", .callback = command, .user_data = w },
        { .action = "cmake.run", .label = "Run CMake target", .callback = command, .user_data = w },
    };
    if (!w->root_sub || !w->fs_sub ||
        !sol_plugin_register_command(ctx, &commands[0]) ||
        !sol_plugin_register_command(ctx, &commands[1]) ||
        !sol_plugin_register_command(ctx, &commands[2])) return false;
    SolUISystem *ui = sol_plugin_ui(ctx);
    rescan(w, ui ? sol_ui_system_file_tree_root(ui) : NULL);
    return true;
}

void cmake_workflow_unload(SolPluginCtx *ctx)
{
    (void)ctx;
}
