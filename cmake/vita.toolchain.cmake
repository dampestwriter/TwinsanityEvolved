# cmake/vita.toolchain.cmake -- the PlayStation Vita lane: VitaSDK's own toolchain file (arm-vita-eabi
# GCC, newlib, armv7-a / Thumb-2, 32-bit pointers) plus the two definitions this tree's C++17 needs from newlib. Used by
# the vita-* presets only (CMakePresets.json); nothing here is read by the macOS / Linux / Windows configurations.
#
#   VITASDK   the SDK root ($VITASDK in the environment, else /usr/local/vitasdk)
#
# The wasm32 half of the build (wasi-sdk clang, wabt's wasm2c, the Python generators) runs on the HOST as on every lane;
# only the C/C++ that becomes the eboot (the wasm2c module, the wasm2c runtime, platform/nativeee, the runner, the
# engine's archives) is compiled by this toolchain.
if(NOT DEFINED ENV{VITASDK})
    if(EXISTS "/usr/local/vitasdk/share/vita.toolchain.cmake")
        set(ENV{VITASDK} "/usr/local/vitasdk")
    else()
        message(FATAL_ERROR "TwinsanityEvolved: VITASDK is not set and /usr/local/vitasdk has no vita.toolchain.cmake (install the VitaSDK)")
    endif()
endif()
include("$ENV{VITASDK}/share/vita.toolchain.cmake")

# newlib hides the POSIX names this tree uses (pread, fseeko, fileno, posix_memalign, strcasecmp) under a strict
# -std=c++17 / -std=c99; _GNU_SOURCE opens them for every TU of the lane (the engine's own CMake does the same for its
# host arm). ASM is the GNU assembler of the same GCC (runner/te_fiber_switch.S has the AAPCS32 arm).
set(CMAKE_C_FLAGS_INIT "-D_GNU_SOURCE")
set(CMAKE_CXX_FLAGS_INIT "-D_GNU_SOURCE")
set(CMAKE_ASM_FLAGS_INIT "")
# The SDK's toolchain file fixes the Release flags in the cache (-O3 -DNDEBUG); the tree's own per-target options apply
# on top as on the other lanes.
set(TE_VITA ON)
