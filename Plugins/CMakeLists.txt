# Plugins/CMakeLists.txt
# All tree-sitter language plugins.  Each is a MODULE (.dylib/.so) that
# loads into sol at runtime via the plugin manager.

# On macOS, CMake defaults MODULE suffix to .so but dlopen-based plugin
# loaders expect .dylib (matching sol_platform_dynamic_library_extension).
if(APPLE)
    set(CMAKE_SHARED_MODULE_SUFFIX ".dylib")
endif()

add_subdirectory(sol-plugin-c)
add_subdirectory(sol-plugin-cpp)
add_subdirectory(sol-plugin-python)
add_subdirectory(sol-plugin-javascript)
add_subdirectory(sol-plugin-typescript)
add_subdirectory(sol-plugin-json)
add_subdirectory(sol-plugin-html)
add_subdirectory(sol-plugin-markdown)
add_subdirectory(sol-plugin-css)
add_subdirectory(sol-plugin-cmake)
