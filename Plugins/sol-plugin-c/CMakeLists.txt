cmake_minimum_required(VERSION 3.20)
project(sol_plugin_c C)

# ---- Grammar sources ------------------------------------------------
set(GRAMMAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-c/src)
set(GRAMMAR_SOURCES ${GRAMMAR_DIR}/parser.c)
if(EXISTS "${GRAMMAR_DIR}/scanner.c")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.c)
elseif(EXISTS "${GRAMMAR_DIR}/scanner.cc")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.cc)
    set_source_files_properties(${GRAMMAR_DIR}/scanner.cc PROPERTIES LANGUAGE CXX)
endif()

# ---- Plugin target --------------------------------------------------
add_library(sol_plugin_c MODULE
    src/plugin.c
    ${GRAMMAR_SOURCES}
)

set_target_properties(sol_plugin_c PROPERTIES
    PREFIX    ""
    OUTPUT_NAME "sol-plugin-c"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/plugins
)

target_include_directories(sol_plugin_c PRIVATE
    ${GRAMMAR_DIR}
    ${CMAKE_SOURCE_DIR}/sol/include
)

sol_embed_highlights_scm(sol_plugin_c
    "${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-c/queries/highlights.scm"
    "${CMAKE_CURRENT_BINARY_DIR}")

# Generated grammar files produce many warnings — silence them.
set_source_files_properties(${GRAMMAR_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w")

# Allow undefined symbols resolved from the host process at load time.
if(APPLE)
    target_link_options(sol_plugin_c PRIVATE -undefined dynamic_lookup)
endif()
