cmake_minimum_required(VERSION 3.16)

project(lab4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(parent parent.c)
add_executable(child child.c)

target_link_libraries(parent PRIVATE Threads::Threads)
target_link_libraries(child PRIVATE Threads::Threads)

target_compile_options(parent PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(child PRIVATE -Wall -Wextra -Wpedantic)