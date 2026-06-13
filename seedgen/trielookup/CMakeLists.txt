cmake_minimum_required(VERSION 3.20)
project(trielookup CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(trielookup
    ../src/trielookup_main.cpp
    ../src/trielookup.cpp
    ../src/mmap_file.cpp
    ../src/tower_rooms.cpp
)

target_include_directories(trielookup PRIVATE ../src)

if(MSVC)
    target_compile_options(trielookup PRIVATE /W4)
else()
    target_compile_options(trielookup PRIVATE -Wall -Wextra)
endif()
