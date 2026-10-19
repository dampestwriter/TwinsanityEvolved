# cmake/windows-clang-cl.toolchain.cmake -- the Windows lane's toolchain file.
#
# Loaded ONLY by the windows-clang-cl-* presets in CMakePresets.json (`cmake --preset windows-clang-cl-release`); the
# macOS and Linux configurations never read it. It is also what the port forwards to the engine's ExternalProject
# (CMakeLists.txt: CMAKE_TOOLCHAIN_FILE is forwarded when set), so the engine's archives and the port's final link are
# built by the same front end with the same flag spelling and the same vcpkg libraries -- the shape of the engine's own
# `windows-release` preset (engine/CMakePresets.json), reused rather than restated:
#
#   compiler   clang-cl (LLVM's cl-compatible driver: VS 2022 Build Tools' "C++ Clang tools for Windows", or an LLVM
#              release on PATH). cl.exe is NOT a lane: runner/te_fiber_win64.c is GNU inline asm and the game's
#              arithmetic contract needs -fwrapv / -ffp-contract=off (the engine's windows-msvc-dev is compile-check only).
#   launcher   engine/tools/build/clang_cl_shim.py -- the trees say `-Wall -Wextra -O2 -ffp-contract=off ...` (GNU
#              spelling); the shim rewrites each flag for the cl front end (`-Wall` -> `/clang:-Wall`, `-O2` -> `/O2`,
#              `-f*` -> `/clang:-f*`) so that it keeps its meaning instead of being "unknown argument ignored". Needs
#              `python` on PATH (python.org / winget Python 3.8+; `python3` is not a Windows name).
#   libraries  vcpkg in manifest mode (vcpkg.json: sdl2, ffmpeg[avcodec,swscale]) through its toolchain file, chain-loaded
#              from here -- VCPKG_ROOT must point at a vcpkg checkout (`git clone https://github.com/microsoft/vcpkg;
#              .\bootstrap-vcpkg.bat`). Triplet x64-windows (dynamic: the DLLs are copied beside the .exe by
#              VCPKG_APPLOCAL_DEPS; tools/ci/release_dist.ps1 ships them).
#
# Rejected on sight: a non-Windows host (the macOS build must never pick this file up by accident).
if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "TwinsanityEvolved: cmake/windows-clang-cl.toolchain.cmake is the Windows lane's toolchain; on this host configure without a preset (README) or use a macOS/Linux shape")
endif()
include_guard(GLOBAL)

# A native build: CMAKE_SYSTEM_NAME is left to CMake (setting it here would mark the build as cross-compiling and change
# how try_run / vcpkg behave).

# The front end. A full path on PATH resolution is left to CMake (it records the resolved path in the cache).
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-cl)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang-cl)
endif()

# The shim: the engine's copy (the vendored engine/ tree, or TE_ENGINE_DIR when a developer builds against a separate
# engine checkout -- the same variable CMakeLists.txt reads).
get_filename_component(_te_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(TE_ENGINE_DIR)
    set(_te_engine "${TE_ENGINE_DIR}")
else()
    set(_te_engine "${_te_root}/engine")
endif()
set(_te_shim "${_te_engine}/tools/build/clang_cl_shim.py")
if(NOT EXISTS "${_te_shim}")
    message(FATAL_ERROR "TwinsanityEvolved: ${_te_shim} missing -- the clone is incomplete (the engine/ tree carries the clang-cl shim), or TE_ENGINE_DIR points at the wrong place")
endif()
set(CMAKE_C_COMPILER_LAUNCHER "python;${_te_shim}")
set(CMAKE_CXX_COMPILER_LAUNCHER "python;${_te_shim}")

# vcpkg, chain-loaded. (Setting CMAKE_TOOLCHAIN_FILE to vcpkg's file and VCPKG_CHAINLOAD_TOOLCHAIN_FILE to this one would
# invert the order; including vcpkg's file from here keeps ONE toolchain file to forward to the engine.)
if(NOT DEFINED ENV{VCPKG_ROOT} OR NOT EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "TwinsanityEvolved: VCPKG_ROOT must point at a vcpkg checkout (git clone https://github.com/microsoft/vcpkg; .\\bootstrap-vcpkg.bat)")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet (the Windows lane: dynamic x64)")
endif()
if(NOT DEFINED VCPKG_APPLOCAL_DEPS)
    set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL "copy the vcpkg DLLs beside every built executable")
endif()
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
