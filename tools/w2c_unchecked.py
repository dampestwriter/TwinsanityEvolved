#!/usr/bin/env python3
"""w2c_unchecked.py -- let a build compile the module's loads and stores without their bounds checks (the Vita lane).

wasm2c's runtime refuses guard-page memory on a 32-bit target, so a 32-bit build checks every load and store against
the memory's size: a 64-bit add and compare with the size read from memory, on the hottest path the game has. This
rewrites the ONE macro the default-page memory accesses go through in the generated `game-impl.h`, so that a build
defining TE_W2C_UNCHECKED_MEMORY skips the check (the bulk copies and fills keep their range checks). A build that does
not define it compiles exactly what wasm2c wrote.

Idempotent; fails loudly if the header no longer has the expected shape (a wabt upgrade).

    w2c_unchecked.py build/w2c/game-impl.h
"""
import sys
from pathlib import Path

MARK = "// te: the per-access bounds check can be compiled out (tools/w2c_unchecked.py)"
OLD = """#if WASM_RT_MEMCHECK_GUARD_PAGES
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);
#else"""
NEW = """%s
#if WASM_RT_MEMCHECK_GUARD_PAGES || defined(TE_W2C_UNCHECKED_MEMORY)
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);
#else""" % MARK


def main():
    p = Path(sys.argv[1])
    s = p.read_text()
    if MARK in s:
        return 0
    if s.count(OLD) != 1:
        sys.stderr.write("w2c_unchecked: game-impl.h does not have the expected MEMCHECK_DEFAULT32 shape\n")
        return 1
    p.write_text(s.replace(OLD, NEW, 1))
    print("w2c_unchecked: the bounds check made optional in", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
