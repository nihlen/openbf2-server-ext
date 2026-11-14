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
#   serverext_qjs   QuickJS-NG (third_party/quickjs-ng, C), the engine of the admin channel's scripts.
#   serverext_script  the script host (src/Script.cpp): engine-free, unit-tested against a mock world.
#   serverext_tests unit tests for serverext_core.
#   serverext_admin_tests  unit tests for the admin channel, TLS included.
#   serverext_script_tests  unit tests for the script host.
#
# The admin channel needs OpenSSL (libssl and libcrypto, linked dynamically; 1.1.1 or 3.x, for TLS 1.3).
# QuickJS-NG is linked statically and adds no shared library.
#
# Linux x86-64 only for now. A Windows build (serverext.dll) needs a Winsock IoThread and a
# non-pipe wake-up in Channel; the platform split is already IoThread_<platform>.cpp.

project(serverext LANGUAGES C CXX)

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

# QuickJS-NG with one local change (third_party/quickjs-ng/README.md), built as its own CMakeLists.txt builds
# the library: C11 with GNU extensions, _GNU_SOURCE, unsigned char and its warning set. NDEBUG in
# every build type: its asserts must never abort a game server. Hidden visibility keeps its JS_*
# symbols out of libserverext.so's exports.
set(SERVEREXT_QJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs-ng")
add_library(serverext_qjs STATIC
    "${SERVEREXT_QJS_DIR}/dtoa.c"
    "${SERVEREXT_QJS_DIR}/libregexp.c"
    "${SERVEREXT_QJS_DIR}/libunicode.c"
    "${SERVEREXT_QJS_DIR}/quickjs.c")
set_target_properties(serverext_qjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
# JS_INTERRUPT_COUNTER_INIT: how many loop rounds and calls pass between two checks of a script's time budget
# (upstream 10000; the one local change in quickjs.c makes it settable). 100 keeps a loop around a slow built-in
# from running far past the budget.
target_compile_definitions(serverext_qjs PRIVATE _GNU_SOURCE QUICKJS_NG_BUILD NDEBUG JS_INTERRUPT_COUNTER_INIT=100)
target_compile_options(serverext_qjs PRIVATE
    -Wall -Wextra -Wformat=2 -Wno-implicit-fallthrough -Wno-sign-compare -Wno-missing-field-initializers
    -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-result -Wno-stringop-truncation -Wno-array-bounds
    -funsigned-char -fvisibility=hidden -pthread)
target_include_directories(serverext_qjs SYSTEM PUBLIC "${SERVEREXT_QJS_DIR}")
target_link_libraries(serverext_qjs PUBLIC m)

add_library(serverext_script STATIC src/Script.cpp)
target_include_directories(serverext_script PUBLIC src)
target_compile_options(serverext_script PRIVATE ${SERVEREXT_COMMON_FLAGS})
target_link_libraries(serverext_script PUBLIC serverext_qjs)

if(NOT EXISTS "${OPENBF2_SRC}/Code/BF2/Game/GameServer/IServerExtension.h")
    message(FATAL_ERROR "OPENBF2_SRC (${OPENBF2_SRC}) must point at an OpenBF2 checkout with IServerExtension.h")
endif()
set(BF2_ROOT "${OPENBF2_SRC}/Code/BF2")

add_library(serverext MODULE
    src/Module.cpp
    src/Admin.cpp
    src/IoThread_posix.cpp)
target_link_libraries(serverext PRIVATE serverext_admin_io serverext_script serverext_core)
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

    add_executable(serverext_script_tests tests/ScriptTests.cpp)
    target_link_libraries(serverext_script_tests PRIVATE serverext_script)
    target_compile_options(serverext_script_tests PRIVATE ${SERVEREXT_COMMON_FLAGS})
    target_link_options(serverext_script_tests PRIVATE -pthread)
    add_test(NAME serverext_script_tests COMMAND serverext_script_tests)
endif()
