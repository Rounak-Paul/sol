cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(QuasarEngine LANGUAGES C CXX OBJC)
else()
    project(QuasarEngine LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    enable_testing()
endif()

# MSVC runtime library
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Dependencies
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# Platform definitions
if(APPLE)
    add_compile_definitions(QS_PLATFORM_APPLE)
elseif(WIN32)
    add_compile_definitions(QS_PLATFORM_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX)
    add_compile_definitions(QS_PLATFORM_LINUX)
endif()

# Vendors
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(vendors/causality)

# causality is a static library — its PRIVATE dependencies don't propagate
# through CMake's dependency graph, so the final executable must link
# shaderc and its deps directly on Linux.
if(UNIX AND NOT APPLE)
    find_library(SHADERC_LIB     NAMES shaderc_combined        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(GLSLANG_LIB     NAMES glslang                 PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_LIB       NAMES SPIRV                   PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_TOOLS_LIB NAMES SPIRV-Tools             PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_TOOLS_OPT NAMES SPIRV-Tools-opt         PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(GLSLANG_RES_LIB NAMES glslang-default-resource-limits PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    set(CAUSALITY_LINUX_DEPS
        ${SHADERC_LIB} ${GLSLANG_LIB} ${SPIRV_LIB}
        ${SPIRV_TOOLS_OPT} ${SPIRV_TOOLS_LIB} ${GLSLANG_RES_LIB})
endif()

# ---- tree-sitter runtime -------------------------------------------
# A single static library built from tree-sitter's unity-build entry
# point.  Linked into sol so it can invoke the parser at runtime.
add_library(tree_sitter_runtime STATIC
    ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/src/lib.c
)
target_include_directories(tree_sitter_runtime
    PUBLIC  ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/src
)
set_target_properties(tree_sitter_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Application
add_subdirectory(sol)

# Plugins (tree-sitter language plugins, built as .dylib/.so MODULEs)
add_subdirectory(plugins)
