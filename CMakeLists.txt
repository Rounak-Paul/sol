cmake_minimum_required(VERSION 3.16)
project(sol VERSION 0.1.0 LANGUAGES C)

# C11 standard required
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(SOL_BUILD_TESTS "Build tests" OFF)
option(SOL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(SOL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(SOL_STATIC_LINK "Build static executable" OFF)

# Platform detection
if(WIN32)
    set(SOL_PLATFORM "windows")
    add_compile_definitions(SOL_PLATFORM_WINDOWS=1)
elseif(APPLE)
    set(SOL_PLATFORM "macos")
    add_compile_definitions(SOL_PLATFORM_MACOS=1 SOL_PLATFORM_POSIX=1)
elseif(UNIX)
    set(SOL_PLATFORM "linux")
    add_compile_definitions(SOL_PLATFORM_LINUX=1 SOL_PLATFORM_POSIX=1)
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O2)
    endif()
    
    if(SOL_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    
    if(SOL_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
elseif(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Zi /Od)
    else()
        add_compile_options(/O2)
    endif()
endif()

# Source files
set(SOL_SOURCES
    src/main.c
    src/core/arena.c
    src/core/array.c
    src/core/hashmap.c
    src/core/intern.c
    src/core/utf8.c
    src/core/path.c
    src/core/file.c
    src/core/time.c
    src/core/glob.c
    src/core/fuzzy.c
    src/buffer/piece_table.c
    src/buffer/buffer.c
    src/buffer/cursor.c
    src/undo/undo_tree.c
    src/view/view.c
    src/keybind/keymap.c
    src/keybind/keybind.c
    src/keybind/flow.c
    src/command/command.c
    src/ui/filetree.c
    src/ui/editor_view.c
    src/ui/statusbar.c
    src/ui/tabbar.c
    src/ui/palette.c
    src/ui/dialog.c
    src/ui/file_picker.c
    src/ui/panel.c
    src/terminal/terminal.c
    src/terminal/terminal_ui.c
    src/lsp/lsp.c
    src/highlight/highlight.c
    src/git/git.c
    src/config/config.c
    src/theme/theme.c
    src/plugin/plugin.c
)

# Header files (for IDEs)
set(SOL_HEADERS
    src/sol.h
)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendors/tuih
)

# Main executable
add_executable(sol ${SOL_SOURCES} ${SOL_HEADERS})

# Link libraries
if(WIN32)
    target_link_libraries(sol PRIVATE ws2_32 shlwapi)
elseif(APPLE)
    # macOS specific
elseif(UNIX)
    target_link_libraries(sol PRIVATE m dl pthread)
    
    # PTY support
    find_library(UTIL_LIB util)
    if(UTIL_LIB)
        target_link_libraries(sol PRIVATE ${UTIL_LIB})
    endif()
endif()

# Static linking
if(SOL_STATIC_LINK)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        target_link_options(sol PRIVATE -static)
    endif()
endif()

# Plugin support - example plugin (disabled until API is complete)
# add_library(sol_example_plugin SHARED plugins/example/plugin.c)
# target_include_directories(sol_example_plugin PRIVATE src)
# set_target_properties(sol_example_plugin PROPERTIES PREFIX "")

# Tests
if(SOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install rules
install(TARGETS sol DESTINATION bin)
install(DIRECTORY share/ DESTINATION share/sol)

# Print configuration
message(STATUS "")
message(STATUS "Sol IDE Configuration:")
message(STATUS "  Platform: ${SOL_PLATFORM}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER_ID}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
