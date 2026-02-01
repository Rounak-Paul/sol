cmake_minimum_required(VERSION 3.20)
project(sol LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UI Library
add_subdirectory(src/vendors/tvk)

set(SRCS
    src/main.cpp
    src/core/application.cpp
)

add_executable(sol ${SRCS})

target_include_directories(sol PRIVATE 
    src/vendors/tvk/include
    src/core
)

target_link_libraries(sol PRIVATE tinyvk)
