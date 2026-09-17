// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#define main sol_application_main
#include "../src/main.c"
#undef main

#include "../src/ui/sol_ui_internal.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "project test failed at %d: %s\n", __LINE__, #value); \
    return 1; \
} } while (0)

/** Drive real project jobs, observers and rendering without injecting OS input. */
static void project_test_frame(SolProjectHost *host)
{
    for (SolAppContext *app = host->projects; app; app = app->next) {
        sol_system_begin_frame(app->systems);
        sol_ui_system_tick(app->ui);
        sol_system_pump_events(app->systems, 128);
    }
    ca_instance_request_frame_after(host->instance, 0.001);
    ca_instance_wake();
    ca_instance_tick(host->instance);
    sol_project_apply_requests(host);
}

/** Exercise real GPU project lifetimes using two disposable folder arguments. */
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    SolProjectHost host = { .argc = argc, .argv = argv };
    host.instance = ca_instance_create(&(Ca_InstanceDesc){ .app_name = "Sol project tests" });
    CHECK(host.instance);
    host.window = ca_window_create(host.instance, &(Ca_WindowDesc){
        .title = "Sol project tests", .width = 1080, .height = 720,
    });
    CHECK(host.window);
    SolAppContext *a = sol_project_create(&host, argv[1]);
    CHECK(a);
    a->settings.autosave_enabled = false;
    sol_project_activate(&host, a);
    project_test_frame(&host);
    SolBufferId a_id = sol_text_buffer_open_empty(a->buffers, "A draft", sol_text_view_render);
    CHECK(a_id);
    CHECK(sol_buffer_set_active_leaf_buffer(a->buffers, a_id));
    SolTextBuffer *a_text = sol_text_buffer_active(a->buffers);
    CHECK(a_text);
    sol_text_buffer_insert_codepoint(a_text, 'A');
    CHECK(sol_project_dirty_count(a) == 1);
    SolTerminal *a_term = sol_terminal_manager_new_tab(a->terminal_mgr, argv[1]);
    CHECK(a_term);
    sol_terminal_manager_set_visible(a->terminal_mgr, true);
    sol_ui_search_window_open_files(a->ui);

    SolAppContext *b = sol_project_create(&host, argv[2]);
    CHECK(b && a->systems != b->systems && a->events != b->events);
    CHECK(a->buffers != b->buffers && a->terminal_mgr != b->terminal_mgr);
    CHECK(sol_buffer_syntax_registry(a->buffers) != sol_buffer_syntax_registry(b->buffers));
    CHECK(sol_system_get_service(a->systems, "git.plugin.state") != sol_system_get_service(b->systems, "git.plugin.state"));
    sol_project_activate(&host, b);
    project_test_frame(&host);
    CHECK(sol_buffer_count(b->buffers) == 0);
    CHECK(sol_terminal_manager_count(b->terminal_mgr) == 0);
    CHECK(sol_ui_system_is_active(b->ui) && !sol_ui_system_is_active(a->ui));
    sol_ui_search_window_open_contents(b->ui);
    SolBufferId b_id = sol_text_buffer_open_empty(b->buffers, "B draft", sol_text_view_render);
    CHECK(b_id);
    CHECK(sol_buffer_set_active_leaf_buffer(b->buffers, b_id));
    sol_text_buffer_insert_codepoint(sol_text_buffer_active(b->buffers), 'B');
    for (int i = 0; i < 12; ++i) {
        sol_project_command(host.active, i % 2 ? "project.next" : "project.previous");
        sol_project_apply_requests(&host);
        project_test_frame(&host);
        CHECK(sol_project_dirty_count(a) == 1 && sol_project_dirty_count(b) == 1);
        CHECK(sol_terminal_manager_count(a->terminal_mgr) == 1);
        CHECK(sol_terminal_manager_count(b->terminal_mgr) == 0);
    }
    sol_project_activate(&host, a);
    CHECK(sol_text_buffer_active(a->buffers) == a_text);
    CHECK(sol_terminal_manager_active(a->terminal_mgr) == a_term);
    CHECK(strcmp(sol_ui_system_file_tree_root(a->ui), argv[1]) == 0);
    CHECK(strcmp(sol_ui_system_file_tree_root(b->ui), argv[2]) == 0);

    /* Simulate confirmed closure at the same frame boundary as the host. */
    host.close_pending = b;
    sol_project_apply_requests(&host);
    project_test_frame(&host);
    CHECK(host.projects == a && !a->next && host.active == a);
    CHECK(sol_text_buffer_active(a->buffers) == a_text);
    CHECK(sol_terminal_manager_active(a->terminal_mgr) == a_term);
    host.close_pending = a;
    sol_project_apply_requests(&host);
    project_test_frame(&host);
    CHECK(host.projects && host.projects != a && host.active == host.projects);
    CHECK(sol_buffer_count(host.active->buffers) == 0);
    CHECK(sol_terminal_manager_count(host.active->terminal_mgr) == 0);
    sol_input_router_destroy(host.router);
    sol_crash_track_events(NULL);
    sol_project_destroy(host.projects);
    ca_instance_destroy(host.instance);
    puts("project runtime isolation, switching and teardown passed");
    return 0;
}
