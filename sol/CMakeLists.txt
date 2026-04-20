file(GLOB_RECURSE SOL_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(sol ${SOL_SOURCES})

target_include_directories(sol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(sol PRIVATE causality Threads::Threads ${CMAKE_DL_LIBS})
