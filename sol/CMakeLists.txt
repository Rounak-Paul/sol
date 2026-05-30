file(GLOB_RECURSE SOL_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(sol ${SOL_SOURCES})

target_include_directories(sol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(sol PRIVATE causality Threads::Threads ${CMAKE_DL_LIBS})

# ---- Tests --------------------------------------------------------------
# The rope is self-contained (libc + POSIX mmap) and is the highest-risk
# piece of new code, so it gets a dedicated unit-test executable wired
# into CTest. Run with `ctest --test-dir build` from the build directory.
if(BUILD_TESTING OR PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_executable(sol_rope_tests
        tests/test_rope.c
        src/core/sol_rope.c
        src/core/sol_platform.c
    )
    target_include_directories(sol_rope_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME sol_rope_tests COMMAND sol_rope_tests)
endif()
