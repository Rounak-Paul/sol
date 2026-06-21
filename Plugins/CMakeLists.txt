# Plugins/CMakeLists.txt
# All tree-sitter language plugins.  Each is a MODULE (.dylib/.so) that
# loads into sol at runtime via the plugin manager.

# On macOS, CMake defaults MODULE suffix to .so but dlopen-based plugin
# loaders expect .dylib (matching sol_platform_dynamic_library_extension).
if(APPLE)
    set(CMAKE_SHARED_MODULE_SUFFIX ".dylib")
endif()

# ---------------------------------------------------------------------------
# sol_embed_highlights_scm(<target> <scm_file> <caller_binary_dir>)
#
# Reads a highlights.scm file and generates highlights_scm.h in the plugin's
# binary directory, containing:
#
#   static const char k_highlights_scm[] = "...escaped content...";
#
# The generated header is added to the target's PRIVATE include path.
# Must be called AFTER the target is defined.  Pass CMAKE_CURRENT_BINARY_DIR
# explicitly (CMake function scope doesn't inherit the caller's bindir).
# ---------------------------------------------------------------------------
function(sol_embed_highlights_scm TARGET SCM_FILE CALLER_BINARY_DIR)
    set(_GEN "${CALLER_BINARY_DIR}/highlights_scm.h")

    # Read and escape the .scm file into a C string literal at configure time.
    file(READ "${SCM_FILE}" _CONTENT)
    # Escape backslashes first (must come before quote-escaping).
    string(REPLACE "\\" "\\\\" _CONTENT "${_CONTENT}")
    # Escape double-quotes.
    string(REPLACE "\"" "\\\"" _CONTENT "${_CONTENT}")
    # Replace literal newlines with \n followed by a new source line so
    # individual string fragments stay short.
    string(REPLACE "\n" "\\n\"\n    \"" _CONTENT "${_CONTENT}")

    file(WRITE "${_GEN}"
        "/* Auto-generated from ${SCM_FILE} — do not edit. */\n"
        "static const char k_highlights_scm[] =\n"
        "    \"${_CONTENT}\";\n"
    )

    target_include_directories(${TARGET} PRIVATE "${CALLER_BINARY_DIR}")
endfunction()

function(sol_configure_plugin_output TARGET)
    set_target_properties(${TARGET} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:sol>/plugins"
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:sol>/plugins"
    )
endfunction()

add_subdirectory(sol-plugin-c)
add_subdirectory(sol-plugin-cpp)
add_subdirectory(sol-plugin-python)
add_subdirectory(sol-plugin-javascript)
add_subdirectory(sol-plugin-typescript)
add_subdirectory(sol-plugin-json)
add_subdirectory(sol-plugin-html)
add_subdirectory(sol-plugin-markdown)
add_subdirectory(sol-plugin-css)
add_subdirectory(sol-plugin-cmake)
add_subdirectory(sol-plugin-git)
add_subdirectory(sol-plugin-bfx)

# On Linux/Windows causality is static, so plugins can't rely on dynamic
# symbol lookup — they must link against the sol executable directly.
if(WIN32)
    foreach(_sol_plugin_target IN ITEMS
        sol_plugin_c
        sol_plugin_cpp
        sol_plugin_python
        sol_plugin_javascript
        sol_plugin_typescript
        sol_plugin_json
        sol_plugin_html
        sol_plugin_markdown
        sol_plugin_css
        sol_plugin_cmake
        sol_plugin_git
        sol_plugin_bfx)
        target_link_libraries(${_sol_plugin_target} PRIVATE sol)
    endforeach()
elseif(UNIX AND NOT APPLE)
    # On Linux, plugins are dlopen'd into the sol process which has all
    # causality symbols in its dynamic symbol table (ENABLE_EXPORTS ON).
    # Allow undefined symbols at link time — they resolve from the host
    # process at dlopen time, the same as -undefined dynamic_lookup on macOS.
    foreach(_sol_plugin_target IN ITEMS
        sol_plugin_c
        sol_plugin_cpp
        sol_plugin_python
        sol_plugin_javascript
        sol_plugin_typescript
        sol_plugin_json
        sol_plugin_html
        sol_plugin_markdown
        sol_plugin_css
        sol_plugin_cmake
        sol_plugin_git
        sol_plugin_bfx)
        target_link_options(${_sol_plugin_target} PRIVATE
            -Wl,--allow-shlib-undefined
            -Wl,--unresolved-symbols=ignore-all)
    endforeach()
endif()
