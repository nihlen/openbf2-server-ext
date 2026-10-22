cmake_minimum_required(VERSION 3.21)

# libserverext: the OpenBF2 dedicated-server module behind the bot channel (docs/protocol.md).
#
#   cmake --preset linux-x64 [-DOPENBF2_SRC=/path/to/OpenBF2]
#   cmake --build --preset linux-x64
#   ctest --preset linux-x64
#
# Layout:
#   serverext_core  engine-free: framing, parsing, config, queues, per-connection protocol state.
#                   Unit-tested without the engine.
#   serverext       the module (libserverext.so): engine glue plus the platform socket thread.
#   serverext_tests unit tests for serverext_core.
#
# Linux x86-64 only for now. A Windows build (serverext.dll) needs a Winsock IoThread and a
# non-pipe wake-up in Channel; the platform split is already IoThread_<platform>.cpp.

project(serverext LANGUAGES CXX)

if(WIN32)
    set(_openbf2_default "C:/Users/Alex/.t3/worktrees/OpenBF2/serverext-r1")
else()
    set(_openbf2_default "/mnt/c/Users/Alex/.t3/worktrees/OpenBF2/serverext-r1")
endif()
set(OPENBF2_SRC "${_openbf2_default}" CACHE PATH "OpenBF2 checkout carrying the R1 engine changes (IServerExtension.h)")
option(SERVEREXT_BUILD_TESTS "Build the engine-free unit tests" ON)

if(WIN32)
    message(FATAL_ERROR "serverext: the Windows build is not implemented yet (needs src/IoThread_win32.cpp "
                        "and a Winsock wake-up in Channel). Use the linux-x64 preset.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SERVEREXT_COMMON_FLAGS -fno-strict-aliasing -fwrapv -ffp-contract=off -pthread -fvisibility=hidden
                           -Wall -Wextra -Wno-unused-parameter)

add_library(serverext_core STATIC
    src/Protocol.cpp
    src/Config.cpp
    src/Channel.cpp
    src/Session.cpp)
target_include_directories(serverext_core PUBLIC src)
target_compile_options(serverext_core PRIVATE ${SERVEREXT_COMMON_FLAGS})

if(NOT EXISTS "${OPENBF2_SRC}/Code/BF2/Game/GameServer/IServerExtension.h")
    message(FATAL_ERROR "OPENBF2_SRC (${OPENBF2_SRC}) must point at an OpenBF2 checkout with IServerExtension.h")
endif()
set(BF2_ROOT "${OPENBF2_SRC}/Code/BF2")

add_library(serverext MODULE
    src/Module.cpp
    src/IoThread_posix.cpp)
target_link_libraries(serverext PRIVATE serverext_core)
target_include_directories(serverext PRIVATE "${BF2_ROOT}")
# The engine image define and the ABI-relevant flags of OpenBF2's linux-x64 objects
# (bf2_configure_cpp_target / bf2_set_platform_compat / bf2_set_image).
target_compile_definitions(serverext PRIVATE DEDICATED=1 _LINUX _GNU_SOURCE)
target_compile_options(serverext PRIVATE
    "-include${BF2_ROOT}/Core/PlatformCompat.h"
    "-include${BF2_ROOT}/Core/BuildConfig.h"
    -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -ffp-contract=off
    -fms-extensions -fpermissive -Wno-narrowing -Wno-unknown-pragmas -Wno-multichar
    -Wno-deprecated-declarations -Wno-write-strings -pthread -fvisibility=hidden)
# Static libstdc++ like bf2, so std::string/std::map layouts match the engine's.
target_link_options(serverext PRIVATE -static-libstdc++ -static-libgcc -pthread "LINKER:--no-undefined")

if(SERVEREXT_BUILD_TESTS)
    enable_testing()
    add_executable(serverext_tests tests/TestMain.cpp)
    target_link_libraries(serverext_tests PRIVATE serverext_core)
    target_compile_options(serverext_tests PRIVATE ${SERVEREXT_COMMON_FLAGS})
    target_link_options(serverext_tests PRIVATE -pthread)
    add_test(NAME serverext_tests COMMAND serverext_tests)
endif()
