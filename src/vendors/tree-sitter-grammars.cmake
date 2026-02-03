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

# Tree-sitter CMake grammar
add_library(tree-sitter-cmake STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cmake/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cmake/src/scanner.c
)
target_include_directories(tree-sitter-cmake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-cmake/src)
target_compile_options(tree-sitter-cmake PRIVATE -w)

# Tree-sitter CSS grammar
add_library(tree-sitter-css STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-css/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-css/src/scanner.c
)
target_include_directories(tree-sitter-css PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-css/src)
target_compile_options(tree-sitter-css PRIVATE -w)

# Tree-sitter HTML grammar
add_library(tree-sitter-html STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-html/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-html/src/scanner.c
)
target_include_directories(tree-sitter-html PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-html/src)
target_compile_options(tree-sitter-html PRIVATE -w)

# Tree-sitter JavaScript grammar
add_library(tree-sitter-javascript STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-javascript/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-javascript/src/scanner.c
)
target_include_directories(tree-sitter-javascript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-javascript/src)
target_compile_options(tree-sitter-javascript PRIVATE -w)

# Tree-sitter JSON grammar
add_library(tree-sitter-json STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-json/src/parser.c
)
target_include_directories(tree-sitter-json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-json/src)
target_compile_options(tree-sitter-json PRIVATE -w)

# Tree-sitter Markdown grammar
add_library(tree-sitter-markdown STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-markdown/tree-sitter-markdown/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-markdown/tree-sitter-markdown/src/scanner.c
)
target_include_directories(tree-sitter-markdown PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-markdown/tree-sitter-markdown/src)
target_compile_options(tree-sitter-markdown PRIVATE -w)

# Tree-sitter TypeScript grammar
add_library(tree-sitter-typescript STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-typescript/typescript/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-typescript/typescript/src/scanner.c
)
target_include_directories(tree-sitter-typescript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/vendors/tree-sitter-typescript/typescript/src)
target_compile_options(tree-sitter-typescript PRIVATE -w)

# Combined grammars library for convenience
add_library(tree-sitter-grammars INTERFACE)
target_link_libraries(tree-sitter-grammars INTERFACE
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-python
    tree-sitter-cmake
    tree-sitter-css
    tree-sitter-html
    tree-sitter-javascript
    tree-sitter-json
    tree-sitter-markdown
    tree-sitter-typescript
)
