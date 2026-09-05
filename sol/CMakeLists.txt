# Short build identifier embedded in crash reports (see sol_crash.c) so a
# report names the exact commit it came from. Falls back to "unknown"
# outside a git checkout (e.g. a source tarball) rather than failing
# configure.
find_package(Git QUIET)
set(SOL_BUILD_VERSION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE SOL_BUILD_VERSION_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE SOL_GIT_RESULT
    )
    if(SOL_GIT_RESULT EQUAL 0 AND SOL_BUILD_VERSION_OUT)
        set(SOL_BUILD_VERSION "${SOL_BUILD_VERSION_OUT}")
    endif()
endif()

file(GLOB_RECURSE SOL_SOURCES CONFIGURE_DEPENDS src/*.c)

set(SOL_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND SOL_PLATFORM_SOURCES src/core/sol_regex.cpp)
endif()

add_executable(sol ${SOL_SOURCES} ${SOL_PLATFORM_SOURCES})

# Export symbols so plugin modules can resolve sol symbols at load time.
# On ELF/Mach-O every process-wide symbol is naturally visible, so
# ENABLE_EXPORTS (which produces the .exp/.lib import archive plugins link
# against on Windows) is enough there. MSVC, unlike GNU/Apple linkers, does
# not export any symbol unless it is explicitly __declspec(dllexport)'d or
# listed in a .def file — none of sol's own sol_* functions are decorated
# that way, so without WINDOWS_EXPORT_ALL_SYMBOLS the generated sol.lib
# would be missing them and every plugin link would fail with LNK2019.
set_target_properties(sol PROPERTIES
    ENABLE_EXPORTS ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(sol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/include
)

target_compile_definitions(sol PRIVATE SOL_BUILD_VERSION="${SOL_BUILD_VERSION}")

target_link_libraries(sol PRIVATE
    causality Threads::Threads ${CMAKE_DL_LIBS} tree_sitter_runtime
    VulkanMemoryAllocator Vulkan::Vulkan libssh2_static
    ${CAUSALITY_LINUX_DEPS}
)

# causality's CA_API macro resolves to dllimport on Windows unless
# causality_EXPORTS is defined, on the assumption that consumers link
# against a shared Quasar.dll that re-exports causality's symbols (see
# vendors/causality/causality/include/ca_api.h). sol instead links the
# static causality archive straight into the executable with no DLL
# boundary in between, so it needs causality_EXPORTS too — otherwise MSVC
# emits calls through a nonexistent import thunk (__imp_ca_*) and the link
# fails. Harmless on non-Windows, where CA_API ignores the macro either way.
if(WIN32)
    target_compile_definitions(sol PRIVATE causality_EXPORTS)
endif()

# The Visual Studio generator does not propagate COMPILE_LANGUAGE generator
# expressions (e.g. $<$<COMPILE_LANGUAGE:C>:...>) from a directory-scoped
# add_compile_options — the flag silently never reaches cl.exe. Apply it
# directly on the target instead. Needed here because main.c, sol_job.c,
# sol_terminal.c, sol_file_watcher.c, and search_window.c include
# <stdatomic.h>, which MSVC gates behind this flag.
if(MSVC)
    target_compile_options(sol PRIVATE /experimental:c11atomics)
endif()

# sol_file_watcher.c uses FSEvents (macOS) / inotify (Linux, libc — no
# extra link) / ReadDirectoryChangesW (Windows, kernel32 — already linked).
if(APPLE)
    find_library(SOL_CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    target_link_libraries(sol PRIVATE ${SOL_CORESERVICES_FRAMEWORK})
endif()

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
    if(WIN32)
        # See the causality_EXPORTS comment on the `sol` target above: this
        # target also links the static causality archive directly.
        target_compile_definitions(sol_command_flow_tests PRIVATE causality_EXPORTS)
    endif()
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
    if(WIN32)
        # See the causality_EXPORTS comment on the `sol` target above: this
        # target also links the static causality archive directly.
        target_compile_definitions(sol_plugin_tests PRIVATE causality_EXPORTS)
    endif()
    if(MSVC)
        # See the /experimental:c11atomics comment on the `sol` target
        # above: this target compiles sol_job.c, which needs it too.
        target_compile_options(sol_plugin_tests PRIVATE /experimental:c11atomics)
    endif()
    add_test(NAME sol_plugin_tests COMMAND sol_plugin_tests)

    # ---- Terminal VT state machine unit tests ---------------------------
    # Whitebox: test_terminal.c #includes sol_terminal.c directly to reach
    # its static VT state machine without spawning a real PTY/SSH channel.
    # Needs libssh2_static (SSH channel code paths, unused at runtime by
    # these tests but referenced) and the real causality lib (ca_instance_wake).
    add_executable(sol_terminal_tests
        tests/test_terminal.c
        src/core/sol_platform.c
    )
    target_include_directories(sol_terminal_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CAUSALITY_TEST_INCLUDES}
    )
    target_compile_definitions(sol_terminal_tests PRIVATE ${SOL_TEST_DEFS})
    target_link_libraries(sol_terminal_tests PRIVATE causality libssh2_static Threads::Threads ${CMAKE_DL_LIBS} ${CAUSALITY_LINUX_DEPS})
    if(WIN32)
        target_compile_definitions(sol_terminal_tests PRIVATE causality_EXPORTS)
    endif()
    if(MSVC)
        target_compile_options(sol_terminal_tests PRIVATE /experimental:c11atomics)
    endif()
    add_test(NAME sol_terminal_tests COMMAND sol_terminal_tests)
endif()
