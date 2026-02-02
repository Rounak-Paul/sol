cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)

# Tree-sitter C grammar
add_library(tree-sitter-c STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-c/src/parser.c
)
target_include_directories(tree-sitter-c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-c/src)
target_compile_options(tree-sitter-c PRIVATE -w)

# Tree-sitter C++ grammar
add_library(tree-sitter-cpp STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cpp/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cpp/src/scanner.c
)
target_include_directories(tree-sitter-cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cpp/src)
target_compile_options(tree-sitter-cpp PRIVATE -w)

# Tree-sitter Python grammar
add_library(tree-sitter-python STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-python/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-python/src/scanner.c
)
target_include_directories(tree-sitter-python PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-python/src)
target_compile_options(tree-sitter-python PRIVATE -w)

# Combined grammars library for convenience
add_library(tree-sitter-grammars INTERFACE)
target_link_libraries(tree-sitter-grammars INTERFACE
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-python
)
