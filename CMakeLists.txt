cmake_minimum_required(VERSION 3.24)
project(Loom CXX)

#####################################################################################################
# Loom - in-RAM jot service.
#
# Targets:
#   loomcore     core + codec + persist + mcp, compiled STRICT (/Wall /WX, -Wall -Wextra -Werror)
#   loom         the service: HTTP + MCP + dashboard
#   loombench    in-process store benchmark, no socket
#   coretest     86 assertions over the store
#   persisttest  29 assertions over WAL/snapshot/replay
#   mcptest      29 assertions over the MCP protocol surface
#
# A LIBRARY plus thin executables, deliberately breaking the ZLibraries convention of copying a
# COMMON_SOURCE list into every tool. Loom has four front ends over one core; compiling that core
# four times was tolerable at phase 1 and would not be by phase 7.
#
# WARNING LEVELS ARE SPLIT, and that split is load-bearing. Core is strict. The files that include
# vendored third-party headers (nlohmann, crow, asio) are relaxed per-file rather than weakening the
# project - which is the whole reason HttpServer.h exposes no crow types and JotJson lives in codec/
# rather than http/.
#####################################################################################################

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /Wall /WX)
    add_compile_options(
        /wd4061   # enum value not explicitly handled by a case label (default covers it)
        /wd4514   # unreferenced inline function removed
        /wd4571   # catch(...) semantics changed since VC 7.1
        /wd4623 /wd4625 /wd4626 /wd5026 /wd5027   # implicitly deleted special members
        /wd4710 /wd4711   # not inlined / selected for inlining
        /wd4820   # padding added after data member
        /wd5045   # Spectre mitigation would be inserted
        /wd5262   # implicit fall-through
    )
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# NOTE: unlike ZLibraries, no -march / -m<isa> flags. Nothing here is hand-vectorized, and pinning
# x86-64 would silently exclude an ARM box for no gain.
#
# NOT YET BUILT ON LINUX. Everything to date is MSVC-only; gcc -Werror will almost certainly have
# opinions, and the fsync path (persist/Journal.cpp, persist/Snapshot.cpp) switches on _WIN32.

#####################################################################################################
# loomcore
#####################################################################################################

add_library(loomcore STATIC
    core/IpAcl.cpp
    core/JotStore.cpp
    core/LoomTime.cpp
    core/Ops.cpp
    core/TagRegistry.cpp
    core/Tokenizer.cpp

    codec/JotJson.cpp
    persist/Journal.cpp
    persist/Snapshot.cpp
    persist/Importer.cpp
    mcp/McpHandler.cpp

    core/FlatJot.h  core/Interner.h  core/IpAcl.h    core/Jot.h      core/JotStore.h
    core/LoomError.h core/LoomTime.h core/Ops.h      core/Query.h
    core/TagRegistry.h core/Tokenizer.h
    codec/JotJson.h
    persist/Journal.h persist/Snapshot.h persist/Importer.h persist/DataLock.h
    mcp/McpHandler.h
)

target_include_directories(loomcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(loomcore PUBLIC Threads::Threads)

# These four include vendored nlohmann, which does not survive /Wall /WX.
set_source_files_properties(
    core/IpAcl.cpp
    codec/JotJson.cpp persist/Journal.cpp persist/Snapshot.cpp persist/Importer.cpp
    mcp/McpHandler.cpp
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W3;/WX-,-w>")

#####################################################################################################
# loom - the service
#####################################################################################################

add_executable(loom main.cpp http/HttpServer.cpp http/HttpServer.h web/Dashboard.h)
target_link_libraries(loom PRIVATE loomcore)

target_include_directories(loom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/asio ${CMAKE_CURRENT_SOURCE_DIR}/vendor/crow)
target_compile_definitions(loom PRIVATE ASIO_STANDALONE CROW_DISABLE_STATIC_DIR)

if(WIN32)
    target_compile_definitions(loom PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(loom PRIVATE ws2_32 mswsock)
endif()

# Crow's headers are the noisiest of the vendored set; quarantine them to this one file.
set_source_files_properties(http/HttpServer.cpp PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W3;/WX-,-w>")

#####################################################################################################
# Benchmark and tests
#####################################################################################################

add_executable(loombench bench/loombench/main.cpp)
target_link_libraries(loombench PRIVATE loomcore)

add_executable(coretest test/coretest/main.cpp)
target_link_libraries(coretest PRIVATE loomcore)

add_executable(persisttest test/persisttest/main.cpp)
target_link_libraries(persisttest PRIVATE loomcore)

add_executable(mcptest test/mcptest/main.cpp)
target_link_libraries(mcptest PRIVATE loomcore)

# persisttest and mcptest include nlohmann directly.
set_source_files_properties(test/persisttest/main.cpp test/mcptest/main.cpp PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W3;/WX-,-w>")

enable_testing()
add_test(NAME core    COMMAND coretest)
add_test(NAME persist COMMAND persisttest)
add_test(NAME mcp     COMMAND mcptest)
