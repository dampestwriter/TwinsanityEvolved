#!/usr/bin/env bash
# Fetch the PINNED wasm32 toolchain into tools/toolchain/ (gitignored) and verify sha256.
#   wasi-sdk 24.0  (clang 18 + wasm-ld + wasi-libc/libc++ sysroot)   -> tools/toolchain/wasi-sdk
#   wabt     1.0.42 (wasm2c, wasm-objdump, wasm-validate)             -> tools/toolchain/wabt
#   libclang 18.1.1 python wheel (gen_vtable_slots.py)           -> tools/toolchain/libclang
# Pins: tools/toolchain/PINS.sha256 -- one line per artifact per host. Hosts (the release workflow's
# matrix + the dev host): macOS arm64 / x86_64, Linux x86_64 / aarch64, Windows x86_64 (Git Bash / MSYS2:
# `uname -s` MINGW64_NT-* / MSYS_NT-*). wabt ships no macOS x86_64 binary for 1.0.42: that host builds
# wasm2c / wasm-objdump from the pinned SOURCE tarball (cmake + a C++17 compiler; ~3 min).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HERE/toolchain"
WASI_SDK_VERSION="24.0"
WASI_SDK_TAG="wasi-sdk-24"
WABT_VERSION="1.0.42"
LIBCLANG_VERSION="18.1.1"

uname_s="$(uname -s)"; uname_m="$(uname -m)"
EXE=""
WABT_FROM_SOURCE=0
case "$uname_s/$uname_m" in
  Darwin/arm64)   WASI_ARCH="arm64-macos";    WABT_ARCH="macos-arm64";  LIBCLANG_TAG="1-py2.py3-none-macosx_11_0_arm64" ;;
  Darwin/x86_64)  WASI_ARCH="x86_64-macos";   WABT_ARCH="";             LIBCLANG_TAG="py2.py3-none-macosx_10_9_x86_64"; WABT_FROM_SOURCE=1 ;;
  Linux/x86_64)   WASI_ARCH="x86_64-linux";   WABT_ARCH="linux-x64";    LIBCLANG_TAG="py2.py3-none-manylinux2010_x86_64" ;;
  Linux/aarch64)  WASI_ARCH="arm64-linux";    WABT_ARCH="linux-arm64";  LIBCLANG_TAG="py2.py3-none-manylinux2014_aarch64" ;;
  MINGW64_NT*/x86_64|MSYS_NT*/x86_64|CYGWIN_NT*/x86_64)
                  WASI_ARCH="x86_64-windows"; WABT_ARCH="windows-x64";  LIBCLANG_TAG="py2.py3-none-win_amd64"; EXE=".exe" ;;
  *) echo "fetch_toolchain: unsupported host $uname_s/$uname_m" >&2; exit 2 ;;
esac
WASI_TGZ="wasi-sdk-${WASI_SDK_VERSION}-${WASI_ARCH}.tar.gz"
WASI_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/${WASI_SDK_TAG}/${WASI_TGZ}"
if [ "$WABT_FROM_SOURCE" = 1 ]; then
  WABT_TGZ="wabt-${WABT_VERSION}.tar.xz"
else
  WABT_TGZ="wabt-${WABT_VERSION}-${WABT_ARCH}.tar.gz"
fi
WABT_URL="https://github.com/WebAssembly/wabt/releases/download/${WABT_VERSION}/${WABT_TGZ}"
LIBCLANG_WHL="libclang-${LIBCLANG_VERSION}-${LIBCLANG_TAG}.whl"
LIBCLANG_URL="https://files.pythonhosted.org/packages/py2.py3/l/libclang/${LIBCLANG_WHL}"

mkdir -p "$DEST"
fetch() { # url file
  if [ ! -f "$DEST/$2" ]; then echo "fetch_toolchain: downloading $2"; curl -fsSL -o "$DEST/$2.part" "$1"; mv "$DEST/$2.part" "$DEST/$2"; fi
}
sha256_of() { # file
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'; else sha256sum "$1" | awk '{print $1}'; fi
}
verify() { # file
  local want; want="$(grep -E "  $1\$" "$DEST/PINS.sha256" | awk '{print $1}' || true)"
  if [ -z "$want" ]; then echo "fetch_toolchain: NO PIN for $1 in PINS.sha256 — refusing" >&2; exit 3; fi
  local got; got="$(sha256_of "$DEST/$1")"
  if [ "$want" != "$got" ]; then echo "fetch_toolchain: sha256 MISMATCH for $1: want $want got $got" >&2; exit 4; fi
  echo "fetch_toolchain: verified $1"
}
fetch "$WASI_URL" "$WASI_TGZ"; verify "$WASI_TGZ"
fetch "$WABT_URL" "$WABT_TGZ";  verify "$WABT_TGZ"
fetch "$LIBCLANG_URL" "$LIBCLANG_WHL"; verify "$LIBCLANG_WHL"

if [ ! -x "$DEST/wasi-sdk/bin/clang$EXE" ]; then
  rm -rf "$DEST/wasi-sdk" "$DEST/wasi-sdk.tmp"; mkdir -p "$DEST/wasi-sdk.tmp"
  tar -xzf "$DEST/$WASI_TGZ" -C "$DEST/wasi-sdk.tmp" --strip-components=1
  mv "$DEST/wasi-sdk.tmp" "$DEST/wasi-sdk"
fi
if [ ! -x "$DEST/wabt/bin/wasm2c$EXE" ]; then
  rm -rf "$DEST/wabt" "$DEST/wabt.tmp"; mkdir -p "$DEST/wabt.tmp"
  if [ "$WABT_FROM_SOURCE" = 1 ]; then
    # No binary release for this host: build wasm2c / wasm-objdump / wasm-validate from the pinned source.
    tar -xJf "$DEST/$WABT_TGZ" -C "$DEST/wabt.tmp" --strip-components=1
    cmake -S "$DEST/wabt.tmp" -B "$DEST/wabt.tmp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
          -DBUILD_TOOLS=ON -DBUILD_LIBWASM=OFF -DWITH_EXCEPTIONS=OFF > "$DEST/wabt.tmp/configure.log" 2>&1
    cmake --build "$DEST/wabt.tmp/build" --target wasm2c wasm-objdump wasm-validate > "$DEST/wabt.tmp/build.log" 2>&1
    mkdir -p "$DEST/wabt.tmp/bin"
    cp "$DEST/wabt.tmp/build/wasm2c" "$DEST/wabt.tmp/build/wasm-objdump" "$DEST/wabt.tmp/build/wasm-validate" "$DEST/wabt.tmp/bin/"
  else
    tar -xzf "$DEST/$WABT_TGZ" -C "$DEST/wabt.tmp" --strip-components=1
  fi
  mv "$DEST/wabt.tmp" "$DEST/wabt"
fi
if [ ! -f "$DEST/libclang/libclang-${LIBCLANG_VERSION}.data/platlib/clang/cindex.py" ]; then
  rm -rf "$DEST/libclang" "$DEST/libclang.tmp"; mkdir -p "$DEST/libclang.tmp"
  unzip -q "$DEST/$LIBCLANG_WHL" -d "$DEST/libclang.tmp"
  mv "$DEST/libclang.tmp" "$DEST/libclang"
fi
"$DEST/wasi-sdk/bin/clang$EXE" --version | head -1
"$DEST/wabt/bin/wasm2c$EXE" --version
echo "fetch_toolchain: OK -> $DEST/{wasi-sdk,wabt,libclang}"
