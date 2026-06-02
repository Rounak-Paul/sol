cmake_minimum_required(VERSION 3.20)
project(sol_plugin_markdown C)

# tree-sitter-markdown contains two grammars:
#   tree-sitter-markdown/       — block grammar  (tree_sitter_markdown)
#   tree-sitter-markdown-inline/ — inline grammar (tree_sitter_markdown_inline)
# Both have files named parser.c + scanner.c, so we use separate OBJECT
# targets to avoid CMake object-name collisions.

set(MD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-markdown)

# ---- Block grammar ---------------------------------------------------
add_library(md_grammar_block OBJECT
    ${MD_ROOT}/tree-sitter-markdown/src/parser.c
    ${MD_ROOT}/tree-sitter-markdown/src/scanner.c
)
target_include_directories(md_grammar_block PRIVATE
    ${MD_ROOT}/tree-sitter-markdown/src
)
set_source_files_properties(
    ${MD_ROOT}/tree-sitter-markdown/src/parser.c
    ${MD_ROOT}/tree-sitter-markdown/src/scanner.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ---- Inline grammar --------------------------------------------------
add_library(md_grammar_inline OBJECT
    ${MD_ROOT}/tree-sitter-markdown-inline/src/parser.c
    ${MD_ROOT}/tree-sitter-markdown-inline/src/scanner.c
)
target_include_directories(md_grammar_inline PRIVATE
    ${MD_ROOT}/tree-sitter-markdown-inline/src
)
set_source_files_properties(
    ${MD_ROOT}/tree-sitter-markdown-inline/src/parser.c
    ${MD_ROOT}/tree-sitter-markdown-inline/src/scanner.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ---- Plugin MODULE ---------------------------------------------------
add_library(sol_plugin_markdown MODULE
    src/plugin.c
    $<TARGET_OBJECTS:md_grammar_block>
    $<TARGET_OBJECTS:md_grammar_inline>
)

set_target_properties(sol_plugin_markdown PROPERTIES
    PREFIX    ""
    OUTPUT_NAME "sol-plugin-markdown"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/plugins
)

target_include_directories(sol_plugin_markdown PRIVATE
    ${MD_ROOT}/tree-sitter-markdown/src
    ${MD_ROOT}/tree-sitter-markdown-inline/src
    ${CMAKE_SOURCE_DIR}/sol/include
)

sol_embed_highlights_scm(sol_plugin_markdown
    "${CMAKE_CURRENT_SOURCE_DIR}/vendors/tree-sitter-markdown/tree-sitter-markdown/queries/highlights.scm"
    "${CMAKE_CURRENT_BINARY_DIR}")

if(APPLE)
    target_link_options(sol_plugin_markdown PRIVATE -undefined dynamic_lookup)
endif()
