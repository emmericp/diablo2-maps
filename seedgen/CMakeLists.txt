cmake_minimum_required(VERSION 3.20)
project(seedgen C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# D2's DLLs are 32-bit Win32 — we must match.
# Configure with:  cmake -B build -A Win32          (MSVC)
#              or: cmake -B build -G "MinGW Makefiles" (MinGW, needs -m32 toolchain)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Must build 32-bit to match D2's Win32 DLLs.\n"
        "Re-run: cmake -B build -A Win32")
endif()

add_executable(mapdump
    src/d2loader.cpp
    src/mapdump.cpp
    src/mapdump_common.cpp
    src/mapdump_full.cpp
    src/mapdump_levelgraph.cpp
    src/mapdump_roomatlas.cpp
    src/mapdump_server.cpp
    src/mapdump_summary.cpp
    src/extractor.cpp
    src/levels_db.cpp
    src/objects_db.cpp
    src/monstats_db.cpp
    src/stringtbl.cpp
    src/tells.cpp
    src/tells_rooms.cpp
    src/tower_rooms.cpp
    thirdparty/miniz/miniz.c
)
target_include_directories(mapdump PRIVATE thirdparty/miniz)
# Suppress noisy warnings in the vendored miniz amalgamation only.
if(MSVC)
    set_source_files_properties(thirdparty/miniz/miniz.c PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(thirdparty/miniz/miniz.c PROPERTIES COMPILE_FLAGS "-w")
endif()

# trielookup is built separately as 64-bit — see seedgen/trielookup/CMakeLists.txt.

add_executable(d2sedit src/d2sedit.cpp)

foreach(target mapdump d2sedit)
    target_include_directories(${target} PRIVATE src)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
