cmake_minimum_required(VERSION 3.21)

# [TEMP] serverext_probe: a throwaway smoke module that proves the OpenBF2 engine changes R1/R1b,
# the Q1 per-tick hook and the R2 heli-entry route at runtime. It is not the serverext module and
# has no socket. Linux x86-64 only. See docs/findings/r1-smoke.md.
#
# Build (WSL):
#   cmake -S probe -B ~/serverext-probe-build -G Ninja -DOPENBF2_SRC=/mnt/c/.../OpenBF2/serverext-r1
#   cmake --build ~/serverext-probe-build
#
# The module compiles against the OpenBF2 headers with the same image define, force-includes and
# ABI-relevant flags as the engine's linux-x64 objects, and makes only typed virtual calls into the
# engine: the bf2 executable exports no symbols to modules.

project(serverext_probe LANGUAGES CXX)

set(OPENBF2_SRC "" CACHE PATH "OpenBF2 checkout carrying the R1 engine changes")
if(NOT EXISTS "${OPENBF2_SRC}/Code/BF2/Game/GameServer/IServerExtension.h")
    message(FATAL_ERROR "OPENBF2_SRC must point at an OpenBF2 checkout with IServerExtension.h")
endif()
set(BF2_ROOT "${OPENBF2_SRC}/Code/BF2")

add_library(serverext_probe MODULE src/Probe.cpp)
set_target_properties(serverext_probe PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_include_directories(serverext_probe PRIVATE "${BF2_ROOT}")
target_compile_definitions(serverext_probe PRIVATE DEDICATED=1 _LINUX _GNU_SOURCE)
# Mirrors bf2_configure_cpp_target / bf2_set_platform_compat / bf2_set_image in OpenBF2's CMakeLists.
target_compile_options(serverext_probe PRIVATE
    "-include${BF2_ROOT}/Core/PlatformCompat.h"
    "-include${BF2_ROOT}/Core/BuildConfig.h"
    -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -ffp-contract=off
    -fms-extensions -fpermissive -Wno-narrowing -Wno-unknown-pragmas -Wno-multichar
    -Wno-deprecated-declarations -Wno-write-strings -pthread
    -fvisibility=hidden)
# Same libstdc++ handling as bf2 (static), so std::string/std::map layouts match the engine's.
target_link_options(serverext_probe PRIVATE -static-libstdc++ -static-libgcc "LINKER:--no-undefined")
