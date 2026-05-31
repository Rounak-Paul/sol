cmake_minimum_required(VERSION 3.20)
project(sol_plugin_typescript C)

# tree-sitter-typescript contains two grammars in subdirectories.
# Both have files named parser.c + scanner.c, so we use separate
# OBJECT library targets to avoid object-name collisions in CMake.

set(TS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-typescript)

# ---- TypeScript grammar (typescript/) --------------------------------
add_library(ts_grammar_typescript OBJECT
    ${TS_ROOT}/typescript/src/parser.c
    ${TS_ROOT}/typescript/src/scanner.c
)
target_include_directories(ts_grammar_typescript PRIVATE
    ${TS_ROOT}/typescript/src
)
set_source_files_properties(
    ${TS_ROOT}/typescript/src/parser.c
    ${TS_ROOT}/typescript/src/scanner.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ---- TSX grammar (tsx/) ----------------------------------------------
add_library(ts_grammar_tsx OBJECT
    ${TS_ROOT}/tsx/src/parser.c
    ${TS_ROOT}/tsx/src/scanner.c
)
target_include_directories(ts_grammar_tsx PRIVATE
    ${TS_ROOT}/tsx/src
)
set_source_files_properties(
    ${TS_ROOT}/tsx/src/parser.c
    ${TS_ROOT}/tsx/src/scanner.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ---- Plugin MODULE ---------------------------------------------------
add_library(sol_plugin_typescript MODULE
    src/plugin.c
    $<TARGET_OBJECTS:ts_grammar_typescript>
    $<TARGET_OBJECTS:ts_grammar_tsx>
)

set_target_properties(sol_plugin_typescript PROPERTIES
    PREFIX    ""
    OUTPUT_NAME "sol-plugin-typescript"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/plugins
)

target_include_directories(sol_plugin_typescript PRIVATE
    ${TS_ROOT}/typescript/src
    ${TS_ROOT}/tsx/src
    ${CMAKE_SOURCE_DIR}/sol/include
)

if(APPLE)
    target_link_options(sol_plugin_typescript PRIVATE -undefined dynamic_lookup)
endif()
