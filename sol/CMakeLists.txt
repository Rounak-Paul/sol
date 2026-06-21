file(GLOB_RECURSE SOL_SOURCES CONFIGURE_DEPENDS src/*.c)

set(SOL_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND SOL_PLATFORM_SOURCES src/core/sol_regex.cpp)
endif()

add_executable(sol ${SOL_SOURCES} ${SOL_PLATFORM_SOURCES})

# Export symbols so plugin modules can resolve sol symbols at load time.
set_target_properties(sol PROPERTIES ENABLE_EXPORTS ON)

target_include_directories(sol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/include
)

target_link_libraries(sol PRIVATE
    causality Threads::Threads ${CMAKE_DL_LIBS} tree_sitter_runtime
    ${CAUSALITY_LINUX_DEPS}
)

# ---- Tests --------------------------------------------------------------
# The rope is self-contained (libc + POSIX mmap) and is the highest-risk
# piece of new code, so it gets a dedicated unit-test executable wired
# into CTest. Run with `ctest --test-dir build` from the build directory.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND (BUILD_TESTING OR PROJECT_IS_TOP_LEVEL))
    enable_testing()

    # ---- Rope tests (existing, minimal deps) ----------------------------
    add_executable(sol_rope_tests
        tests/test_rope.c
        src/core/sol_rope.c
        src/core/sol_platform.c
    )
    target_include_directories(sol_rope_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME sol_rope_tests COMMAND sol_rope_tests)

    # ---- Shared settings for harness-based tests -----------------------
    set(SOL_TEST_DEFS SOL_TESTS_ENABLED=1 SOL_VALIDATION_ENABLED=1)

    # Core sources needed by most test targets (no causality dep).
    set(SOL_CORE_TEST_SRC
        src/core/sol_event.c
        src/core/sol_rope.c
        src/core/sol_platform.c
    )

    # Sources that pull in causality symbols (sol_buffer, sol_text_buffer).
    set(SOL_BUFFER_TEST_SRC
        src/core/sol_buffer.c
        src/core/sol_text_buffer.c
        src/core/sol_syntax.c
        src/core/sol_syntax_highlight.c
        ${SOL_PLATFORM_SOURCES}
    )

    # Include path for causality headers (sol_buffer.c includes causality.h).
    # Tests use a signal stub instead of the full causality dylib.
    set(CAUSALITY_INCLUDE ${CMAKE_SOURCE_DIR}/vendors/causality/causality/include)
    set(CAUSALITY_TEST_INCLUDES ${CAUSALITY_INCLUDE} ${Vulkan_INCLUDE_DIRS})

    # ---- File-tree unit tests ------------------------------------------
    add_executable(sol_file_tree_tests
        tests/test_file_tree.c
        tests/stubs/sol_causality_signals_stub.c
        src/core/sol_file_tree.c
        ${SOL_CORE_TEST_SRC}
    )
    target_include_directories(sol_file_tree_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_file_tree_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_file_tree_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sol_file_tree_tests COMMAND sol_file_tree_tests)

    # ---- Workspace-search unit tests -----------------------------------
    add_executable(sol_search_tests
        tests/test_search.c
        src/core/sol_search.c
        src/core/sol_platform.c
    )
    target_include_directories(sol_search_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(sol_search_tests PRIVATE ${SOL_TEST_DEFS})
    add_test(NAME sol_search_tests COMMAND sol_search_tests)

    # ---- Runtime CSS theme registry -----------------------------------
    add_executable(sol_theme_tests
        tests/test_theme.c
        src/core/sol_theme.c
    )
    target_include_directories(sol_theme_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(sol_theme_tests PRIVATE ${SOL_TEST_DEFS})
    add_test(NAME sol_theme_tests COMMAND sol_theme_tests)

    # ---- Event-bus unit tests -------------------------------------------
    add_executable(sol_event_tests
        tests/test_event.c
        tests/stubs/sol_causality_signals_stub.c
        ${SOL_CORE_TEST_SRC}
    )
    target_include_directories(sol_event_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(sol_event_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_event_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sol_event_tests COMMAND sol_event_tests)

    # ---- Buffer-system unit tests ---------------------------------------
    add_executable(sol_buffer_tests
        tests/test_buffer.c
        tests/stubs/sol_causality_signals_stub.c
        ${SOL_CORE_TEST_SRC}
        ${SOL_BUFFER_TEST_SRC}
    )
    target_include_directories(sol_buffer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_buffer_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_buffer_tests PRIVATE tree_sitter_runtime Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sol_buffer_tests COMMAND sol_buffer_tests)

    # ---- Text-buffer unit tests -----------------------------------------
    add_executable(sol_text_buffer_tests
        tests/test_text_buffer.c
        tests/stubs/sol_causality_signals_stub.c
        ${SOL_CORE_TEST_SRC}
        ${SOL_BUFFER_TEST_SRC}
    )
    target_include_directories(sol_text_buffer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_text_buffer_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_text_buffer_tests PRIVATE tree_sitter_runtime Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sol_text_buffer_tests COMMAND sol_text_buffer_tests)

    # ---- Command-flow unit tests ----------------------------------------
    add_executable(sol_command_flow_tests
        tests/test_command_flow.c
        tests/stubs/sol_ui_bump_stub.c
        src/ui/command_flow.c
        src/core/sol_theme.c
        ${SOL_CORE_TEST_SRC}
    )
    target_include_directories(sol_command_flow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${Vulkan_INCLUDE_DIRS}
    )
    target_compile_definitions(sol_command_flow_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_command_flow_tests PRIVATE causality Threads::Threads ${CMAKE_DL_LIBS} ${CAUSALITY_LINUX_DEPS})
    add_test(NAME sol_command_flow_tests COMMAND sol_command_flow_tests)

    # ---- Integration tests (event+buffer+text-buffer end-to-end) -------
    add_executable(sol_integration_tests
        tests/test_integration.c
        tests/stubs/sol_causality_signals_stub.c
        ${SOL_CORE_TEST_SRC}
        ${SOL_BUFFER_TEST_SRC}
    )
    target_include_directories(sol_integration_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_integration_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_integration_tests PRIVATE tree_sitter_runtime Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME sol_integration_tests COMMAND sol_integration_tests)

    # ---- Plugin system unit tests ---------------------------------------
    # Links the full causality library (needed by status_bar.c render
    # symbols and sol_ui_internal.h signal accessors).  Does NOT use
    # sol_causality_signals_stub.c because causality provides those symbols.
    add_executable(sol_plugin_tests
        tests/test_plugin.c
        tests/stubs/sol_ui_bump_stub.c
        tests/stubs/sol_syntax_stub.c
        tests/stubs/sol_syntax_highlight_stub.c
        src/core/sol_plugin.c
        src/core/sol_system_manager.c
        src/core/sol_event.c
        src/core/sol_rope.c
        src/core/sol_platform.c
        src/core/sol_buffer.c
        src/core/sol_text_buffer.c
        src/core/sol_job.c
        src/core/sol_input.c
        src/core/sol_theme.c
        src/ui/command_flow.c
        src/ui/status_bar.c
    )
    target_include_directories(sol_plugin_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_plugin_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_plugin_tests PRIVATE causality Threads::Threads ${CMAKE_DL_LIBS} ${CAUSALITY_LINUX_DEPS})
    add_test(NAME sol_plugin_tests COMMAND sol_plugin_tests)
endif()
