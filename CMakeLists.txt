cmake_minimum_required(VERSION 3.20)
project(sol LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# UI Library
add_subdirectory(src/vendors/tvk)

# Tree-sitter library (static)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(src/vendors/tree-sitter)

# Tree-sitter grammars
include(src/vendors/tree-sitter-grammars.cmake)

set(SRCS
    src/main.cpp
    src/core/application.cpp
    src/core/event_system.cpp
    src/core/logger.cpp
    src/core/job_system.cpp
    src/core/resource_system.cpp
    src/core/text/rope.cpp
    src/core/text/text_buffer.cpp
    src/core/text/undo_tree.cpp
    src/ui/ui_system.cpp
    src/ui/input/input_mode.cpp
    src/ui/input/standard_mode.cpp
    src/ui/input/input_manager.cpp
    src/ui/layers/menu_bar.cpp
    src/ui/layers/explorer.cpp
    src/ui/layers/status_bar.cpp
    src/ui/layers/workspace.cpp
    src/ui/widgets/syntax_editor.cpp
    src/core/lsp/lsp_client.cpp
    src/core/lsp/lsp_manager.cpp
)

# Platform-specific file dialog and process implementations
if(APPLE)
    list(APPEND SRCS 
        src/core/platform/file_dialog_macos.mm
        src/core/platform/process_macos.cpp
    )
elseif(WIN32)
    list(APPEND SRCS 
        src/core/platform/file_dialog_windows.cpp
        src/core/platform/process_windows.cpp
    )
else()
    list(APPEND SRCS 
        src/core/platform/file_dialog_linux.cpp
        src/core/platform/process_linux.cpp
    )
endif()

add_executable(sol ${SRCS})

target_include_directories(sol PRIVATE 
    src
    src/vendors/tvk/include
    src/vendors/tree-sitter/lib/include
)

target_link_libraries(sol PRIVATE 
    tinyvk 
    tree-sitter
    tree-sitter-grammars
)

# Platform-specific frameworks for file dialogs
if(APPLE)
    target_link_libraries(sol PRIVATE "-framework UniformTypeIdentifiers")
endif()
