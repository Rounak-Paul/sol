cmake_minimum_required(VERSION 3.20)
project(sol_plugin_cmake C)

set(GRAMMAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-cmake/src)
set(GRAMMAR_SOURCES ${GRAMMAR_DIR}/parser.c)
if(EXISTS "${GRAMMAR_DIR}/scanner.c")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.c)
elseif(EXISTS "${GRAMMAR_DIR}/scanner.cc")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.cc)
    set_source_files_properties(${GRAMMAR_DIR}/scanner.cc PROPERTIES LANGUAGE CXX)
endif()

add_library(sol_plugin_cmake MODULE
    src/plugin.c
    ${GRAMMAR_SOURCES}
)

set_target_properties(sol_plugin_cmake PROPERTIES
    PREFIX    ""
    OUTPUT_NAME "sol-plugin-cmake"
)
sol_configure_plugin_output(sol_plugin_cmake)

target_include_directories(sol_plugin_cmake PRIVATE
    ${GRAMMAR_DIR}
    ${CMAKE_SOURCE_DIR}/sol/include
)

sol_embed_highlights_scm(sol_plugin_cmake
    "${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-cmake/queries/highlights.scm"
    "${CMAKE_CURRENT_BINARY_DIR}")

set_source_files_properties(${GRAMMAR_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w")

if(APPLE)
    target_link_options(sol_plugin_cmake PRIVATE -undefined dynamic_lookup)
endif()
