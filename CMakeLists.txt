cmake_minimum_required(VERSION 3.21)

# libserverext: the OpenBF2 dedicated-server module behind the bot channel (docs/protocol.md) and the
# admin channel (docs/findings/admin-phase1.md).
#
#   cmake --preset linux-x64 [-DOPENBF2_SRC=/path/to/OpenBF2]
#   cmake --build --preset linux-x64
#   ctest --preset linux-x64
#
# Layout:
#   serverext_core  engine-free: framing, parsing, config, queues, per-connection protocol state.
#                   Unit-tested without the engine.
#   serverext       the module (libserverext.so): engine glue plus the platform socket thread.
#   serverext_admin_io  the admin channel's I/O thread (sockets, TLS): engine-free, so the TLS test
#                   can run it against a local server.
#   serverext_tests unit tests for serverext_core.
#   serverext_admin_tests  unit tests for the admin channel, TLS included.
#
# The admin channel needs OpenSSL (libssl and libcrypto, linked dynamically; 1.1.1 or 3.x, for TLS 1.3).
#
# Linux x86-64 only for now. A Windows build (serverext.dll) needs a Winsock IoThread and a
# non-pipe wake-up in Channel; the platform split is already IoThread_<platform>.cpp.

project(serverext LANGUAGES CXX)

if(WIN32)
    set(_openbf2_default "C:/Data/Projects/OpenBF2")
else()
    set(_openbf2_default "/mnt/c/Data/Projects/OpenBF2")
endif()
set(OPENBF2_SRC "${_openbf2_default}" CACHE PATH "OpenBF2 checkout carrying the IServerExtension engine changes (branch main of the default checkout)")
option(SERVEREXT_BUILD_TESTS "Build the engine-free unit tests" ON)

if(WIN32)
    message(FATAL_ERROR "serverext: the Windows build is not implemented yet (needs src/IoThread_win32.cpp "
                        "and a Winsock wake-up in Channel). Use the linux-x64 preset.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL 1.1.1 REQUIRED)

set(SERVEREXT_COMMON_FLAGS -fno-strict-aliasing -fwrapv -ffp-contract=off -pthread -fvisibility=hidden
                           -Wall -Wextra -Wno-unused-parameter)

add_library(serverext_core STATIC
    src/Protocol.cpp
    src/Config.cpp
    src/Channel.cpp
    src/Session.cpp
    src/AdminProtocol.cpp
    src/AdminCrypto.cpp
    src/AdminLink.cpp
    src/AdminSession.cpp)
target_include_directories(serverext_core PUBLIC src)
target_compile_options(serverext_core PRIVATE ${SERVEREXT_COMMON_FLAGS})
target_link_libraries(serverext_core PUBLIC OpenSSL::Crypto)

add_library(serverext_admin_io STATIC src/AdminIo_posix.cpp)
target_link_libraries(serverext_admin_io PUBLIC serverext_core OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(serverext_admin_io PRIVATE ${SERVEREXT_COMMON_FLAGS})

if(NOT EXISTS "${OPENBF2_SRC}/Code/BF2/Game/GameServer/IServerExtension.h")
    message(FATAL_ERROR "OPENBF2_SRC (${OPENBF2_SRC}) must point at an OpenBF2 checkout with IServerExtension.h")
endif()
set(BF2_ROOT "${OPENBF2_SRC}/Code/BF2")

add_library(serverext MODULE
    src/Module.cpp
    src/Admin.cpp
    src/IoThread_posix.cpp)
target_link_libraries(serverext PRIVATE serverext_admin_io serverext_core)
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

    add_executable(serverext_admin_tests tests/AdminTests.cpp)
    target_link_libraries(serverext_admin_tests PRIVATE serverext_admin_io serverext_core OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(serverext_admin_tests PRIVATE ${SERVEREXT_COMMON_FLAGS} -Wno-deprecated-declarations)
    target_compile_definitions(serverext_admin_tests PRIVATE SERVEREXT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_options(serverext_admin_tests PRIVATE -pthread)
    add_test(NAME serverext_admin_tests COMMAND serverext_admin_tests)
endif()
