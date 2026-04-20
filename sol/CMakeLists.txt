file(GLOB_RECURSE SOL_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(sol ${SOL_SOURCES})

target_include_directories(sol PRIVATE include)
target_link_libraries(sol PRIVATE causality)
