cmake_minimum_required(VERSION 3.20)
project(sol_plugin_themes C)

add_library(sol_plugin_themes MODULE
    src/plugin.c
)

set_target_properties(sol_plugin_themes PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sol-plugin-themes"
)
sol_configure_plugin_output(sol_plugin_themes)

target_include_directories(sol_plugin_themes PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/sol/include
    ${CMAKE_SOURCE_DIR}/vendors/causality/causality/include
)

if(APPLE)
    target_link_options(sol_plugin_themes PRIVATE -undefined dynamic_lookup)
endif()

if(WIN32)
    target_link_libraries(sol_plugin_themes PRIVATE sol)
elseif(UNIX AND NOT APPLE)
    target_link_options(sol_plugin_themes PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,--unresolved-symbols=ignore-all)
endif()
