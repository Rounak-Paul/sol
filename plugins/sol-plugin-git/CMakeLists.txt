cmake_minimum_required(VERSION 3.20)
project(sol_plugin_git C)

add_library(sol_plugin_git MODULE
    src/plugin.c
    src/git_process.c
    src/git_model.c
    src/git_view.c
)

set_target_properties(sol_plugin_git PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sol-plugin-git"
)
sol_configure_plugin_output(sol_plugin_git)

target_include_directories(sol_plugin_git PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/sol/include
    ${CMAKE_SOURCE_DIR}/vendors/causality/causality/include
)

if(UNIX AND NOT APPLE)
    target_link_libraries(sol_plugin_git PRIVATE m)
endif()

if(APPLE)
    target_link_options(sol_plugin_git PRIVATE -undefined dynamic_lookup)
endif()

# The Visual Studio generator does not propagate COMPILE_LANGUAGE generator
# expressions from a directory-scoped add_compile_options — see the same
# fix on the `sol` target in ../../sol/CMakeLists.txt. plugin.c includes
# <stdatomic.h>, which MSVC gates behind this flag.
if(MSVC)
    target_compile_options(sol_plugin_git PRIVATE /experimental:c11atomics)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_executable(sol_git_plugin_tests
        tests/test_git_model.c
        src/git_model.c
        src/git_process.c
    )
    target_include_directories(sol_git_plugin_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/sol/include
    )
    add_test(NAME sol_git_plugin_tests COMMAND sol_git_plugin_tests)
endif()
