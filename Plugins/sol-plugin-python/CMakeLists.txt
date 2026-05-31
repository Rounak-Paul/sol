cmake_minimum_required(VERSION 3.20)
project(sol_plugin_python C)

set(GRAMMAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-python/src)
set(GRAMMAR_SOURCES ${GRAMMAR_DIR}/parser.c)
if(EXISTS "${GRAMMAR_DIR}/scanner.c")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.c)
elseif(EXISTS "${GRAMMAR_DIR}/scanner.cc")
    list(APPEND GRAMMAR_SOURCES ${GRAMMAR_DIR}/scanner.cc)
    set_source_files_properties(${GRAMMAR_DIR}/scanner.cc PROPERTIES LANGUAGE CXX)
endif()

add_library(sol_plugin_python MODULE
    src/plugin.c
    ${GRAMMAR_SOURCES}
)

set_target_properties(sol_plugin_python PROPERTIES
    PREFIX    ""
    OUTPUT_NAME "sol-plugin-python"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/plugins
)

target_include_directories(sol_plugin_python PRIVATE
    ${GRAMMAR_DIR}
    ${CMAKE_SOURCE_DIR}/sol/include
)

set_source_files_properties(${GRAMMAR_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w")

if(APPLE)
    target_link_options(sol_plugin_python PRIVATE -undefined dynamic_lookup)
endif()
