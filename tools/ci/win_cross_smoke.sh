#!/usr/bin/env bash
# tools/ci/win_cross_smoke.sh -- compile the port's Windows-relevant TUs for x86_64-pc-windows-msvc on a macOS/Linux dev
# host, BEFORE the Windows lane sees them (the engine has the same tool for its tree: engine/tools/build/win_cross_smoke.py).
#
#   tools/ci/win_cross_smoke.sh --build <port build dir> --xwin <xwin splat root> [--clang <clang++>] [--out DIR] [--objects]
#
# What it does: every runner TU (runner/*.c, runner/*.cpp, runner/te_fiber_win64.c -- never te_fiber_switch.S, the POSIX
# arm), every platform/nativeee/host TU, the unit tests (platform/nativeee/tests/*.cpp), the two tool executables
# (tools/judges/graphics_selftest.cpp, tools/oracle/te_pdesc.cpp) and the module (the build dir's wasm2c output game_*.c,
# wabt's wasm-rt runtime, the generated seam / dispatch / vtable / retail tables) is re-issued with
# `--target=x86_64-pc-windows-msvc`, the MSVC CRT + Windows SDK headers from an `xwin splat` (the headers clang-cl sees
# on the runner), the port's own include paths and defines (the generated seam / wasm2c headers come from the POSIX
# build dir given: they are platform-neutral), the Windows lane's UCRT definitions (CMakeLists.txt's if(WIN32) list),
# -Wall -Wextra. C is compiled as C17 as clang-cl does (CMake has no C99 flag for it), against the port's copy of wasm-rt.h
# (the WASM_RT_THREAD_LOCAL hook, generated here as CMakeLists.txt generates it on the lane) with WASM_RT_THREAD_LOCAL
# empty -- te_run.c's compile-time check runs. Syntax-only by default (`--objects` writes COFF objects under --out, and
# links te_fiber_test into a PE32+ when lld-link is on PATH -- not run here). Exit 0 iff every TU compiled. The record
# is <out>/SUMMARY.tsv.
#
# What it does NOT do: run anything, or compile the SDL- / FFmpeg-including TUs (platform/nativeee/host/te_input.cpp,
# platform/nativeee/tests/test_movie.cpp: the Windows lane's SDL2 / FFmpeg headers are vcpkg's, not the dev host's) --
# they are listed as SKIP.
set -u
BUILD=""; XWIN="${PS2R_XWIN_ROOT:-}"; CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang++}"; OUT="/tmp/te_win_cross_smoke"; OBJECTS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD="$2"; shift 2 ;;
        --xwin) XWIN="$2"; shift 2 ;;
        --clang) CLANG="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --objects) OBJECTS=1; shift ;;
        *) echo "win_cross_smoke: unknown argument $1" >&2; exit 2 ;;
    esac
done
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
[ -n "$BUILD" ] && [ -d "$BUILD/gen/seam" ] || { echo "win_cross_smoke: --build must be a configured+built port build dir (gen/seam missing)" >&2; exit 2; }
[ -n "$XWIN" ] && [ -d "$XWIN/crt/include" ] && [ -d "$XWIN/sdk/include/um" ] || { echo "win_cross_smoke: --xwin must be an xwin splat root (crt/, sdk/)" >&2; exit 2; }
[ -x "$CLANG" ] || { echo "win_cross_smoke: clang++ not found at $CLANG (Homebrew LLVM or Apple clang)" >&2; exit 2; }
mkdir -p "$OUT"
CC_C="${CLANG%++}"   # clang++ -> clang
[ -x "$CC_C" ] || CC_C="$CLANG"

# The port's copy of wasm-rt.h (as CMakeLists.txt writes it on the Windows lane: the hook ahead of the storage-class chain)
mkdir -p "$OUT/wasm-rt-win"
python3 - "$HERE/tools/toolchain/wabt/include/wasm-rt.h" "$OUT/wasm-rt-win/wasm-rt.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding="utf-8").read().replace("\r\n", "\n")
needle = "#ifdef WASM_RT_C11_AVAILABLE\n#define WASM_RT_THREAD_LOCAL _Thread_local\n#elif defined(_MSC_VER)"
if needle not in t:
    sys.exit("win_cross_smoke: wasm-rt.h is not the pinned 1.0.42 text (the thread-local hook)")
t = t.replace(needle, "#if defined(WASM_RT_THREAD_LOCAL)\n/* the port's thread-local hook */\n#elif defined(WASM_RT_C11_AVAILABLE)\n#define WASM_RT_THREAD_LOCAL _Thread_local\n#elif defined(_MSC_VER)")
open(dst, "w", encoding="utf-8").write(t)
PY
COMMON=(--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.37 -fsyntax-only -Wall -Wextra
        -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -D_USE_MATH_DEFINES -DNOMINMAX -DWIN32_LEAN_AND_MEAN
        -DTE_HAVE_SDL_INPUT=1 -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1 -DWASM_RT_USE_MMAP=0 -DWASM_RT_THREAD_LOCAL= -DPS2E_DRAW_HAS_BIND_THREAD
        -I"$HERE/runner" -I"$HERE/platform/nativeee/host" -I"$HERE/platform/nativeee" -I"$HERE/platform/nativeee/tests" -I"$HERE/engine"
        -I"$OUT/wasm-rt-win" -I"$BUILD/w2c" -I"$BUILD/gen/seam" -I"$BUILD/gen"
        -I"$HERE/tools/toolchain/wabt/include" -I"$HERE/tools/toolchain/wabt/share/wabt/wasm2c"
        -isystem "$XWIN/crt/include" -isystem "$XWIN/sdk/include/ucrt" -isystem "$XWIN/sdk/include/um" -isystem "$XWIN/sdk/include/shared")
if [ "$OBJECTS" = 1 ]; then COMMON=("${COMMON[@]/-fsyntax-only/-c}"); fi

TUS=()
for f in "$HERE"/runner/*.c "$HERE"/runner/*.cpp "$HERE"/platform/nativeee/host/*.cpp "$HERE"/platform/nativeee/tests/*.cpp \
         "$HERE"/tools/judges/graphics_selftest.cpp "$HERE"/tools/oracle/te_pdesc.cpp; do TUS+=("$f"); done
# The module (tegame): wasm2c's output + runtime and the generated host tables are platform-neutral C; what the Windows lane
# compiles of them is exactly this (-w as CMakeLists.txt: wasm2c's output is not warning-clean by design).
RT="$HERE/tools/toolchain/wabt/share/wabt/wasm2c"
MODULE_TUS=("$RT/wasm-rt-impl.c" "$RT/wasm-rt-mem-impl.c" "$RT/wasm-rt-exceptions-impl.c"
            "$BUILD/gen/seam/seam_host_thunks.c" "$BUILD/gen/eabi_dispatch.c" "$BUILD/gen/vtable/vtable_slots_host.c"
            "$BUILD/gen/retail_functions_host.c")
for f in "$BUILD"/w2c/game_*.c; do [ -f "$f" ] && MODULE_TUS+=("$f"); done
for f in "${MODULE_TUS[@]}"; do [ -f "$f" ] && TUS+=("$f"); done
printf 'tu\tstatus\terrors\tseconds\n' > "$OUT/SUMMARY.tsv"
fail=0; ok=0; skip=0
for f in "${TUS[@]}"; do
    rel="${f#$HERE/}"; rel="${rel#$BUILD/}"
    wflags=()
    case "$f" in "$BUILD"/w2c/*|"$RT"/*|"$BUILD"/gen/*) wflags=(-w) ;; esac   # the module: as the tegame target
    case "$rel" in
        platform/nativeee/host/te_input.cpp) printf '%s\tSKIP\t0\t0\n' "$rel" >> "$OUT/SUMMARY.tsv"; echo "  SKIP          $rel (SDL headers are the runner's vcpkg)"; skip=$((skip+1)); continue ;;
        platform/nativeee/tests/test_movie.cpp) printf '%s\tSKIP\t0\t0\n' "$rel" >> "$OUT/SUMMARY.tsv"; echo "  SKIP          $rel (FFmpeg headers are the runner's vcpkg)"; skip=$((skip+1)); continue ;;
    esac
    log="$OUT/$(echo "$rel" | tr '/' '_').log"
    t0=$(date +%s)
    case "$f" in
        *.c)   cmd=("$CC_C" -std=c17 "${COMMON[@]}" ${wflags[@]+"${wflags[@]}"}) ;;   # clang-cl's C default on the lane
        *.cpp) cmd=("$CLANG" -std=c++17 "${COMMON[@]}" ${wflags[@]+"${wflags[@]}"}) ;;
    esac
    if [ "$OBJECTS" = 1 ]; then cmd+=(-o "$OUT/$(echo "$rel" | tr '/' '_').obj"); fi
    "${cmd[@]}" "$f" > "$log" 2>&1
    rc=$?
    dt=$(( $(date +%s) - t0 ))
    errs=$(grep -c "error:" "$log" || true)
    if [ $rc -eq 0 ]; then ok=$((ok+1)); printf '%s\tOK\t%s\t%s\n' "$rel" "$errs" "$dt" >> "$OUT/SUMMARY.tsv"; echo "  OK   ${dt}s     $rel"
    else fail=$((fail+1)); printf '%s\tFAIL\t%s\t%s\n' "$rel" "$errs" "$dt" >> "$OUT/SUMMARY.tsv"; echo "  FAIL ${dt}s errors=$errs  $rel"; grep "error:" "$log" | head -3 | sed 's/^/        /'; fi
done
if [ "$OBJECTS" = 1 ] && command -v lld-link >/dev/null 2>&1; then
    # te_fiber_test as a PE32+: every Win32 import of the fiber arm resolves against the SDK import libraries.
    if lld-link /nologo /subsystem:console /out:"$OUT/te_fiber_test.exe" /DYNAMICBASE:NO \
        "/libpath:$XWIN/crt/lib/x86_64" "/libpath:$XWIN/sdk/lib/um/x86_64" "/libpath:$XWIN/sdk/lib/ucrt/x86_64" \
        "$OUT/runner_te_fiber_test.c.obj" "$OUT/runner_te_fiber.c.obj" "$OUT/runner_te_fiber_win64.c.obj" \
        kernel32.lib libcmt.lib libucrt.lib libvcruntime.lib > "$OUT/link_te_fiber_test.log" 2>&1
    then echo "  OK   link   te_fiber_test.exe (PE32+, not run here)"; else echo "  FAIL link   te_fiber_test.exe"; head -5 "$OUT/link_te_fiber_test.log" | sed 's/^/        /'; fail=$((fail+1)); fi
fi
echo "win_cross_smoke: ok=$ok fail=$fail skip=$skip -> $OUT/SUMMARY.tsv"
[ $fail -eq 0 ]
