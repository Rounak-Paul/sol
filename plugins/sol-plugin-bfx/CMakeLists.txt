cmake_minimum_required(VERSION 3.20)
project(sol_plugin_bfx C)

add_library(sol_plugin_bfx MODULE
    src/plugin.c
)

set_target_properties(sol_plugin_bfx PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sol-plugin-bfx"
)
sol_configure_plugin_output(sol_plugin_bfx)

target_include_directories(sol_plugin_bfx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/sol/include
    ${CMAKE_SOURCE_DIR}/vendors/causality/causality/include
    ${Vulkan_INCLUDE_DIRS}
)

if(APPLE)
    target_link_options(sol_plugin_bfx PRIVATE -undefined dynamic_lookup)
endif()

if(WIN32)
    target_link_libraries(sol_plugin_bfx PRIVATE sol)
elseif(UNIX AND NOT APPLE)
    target_link_options(sol_plugin_bfx PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,--unresolved-symbols=ignore-all)
endif()
