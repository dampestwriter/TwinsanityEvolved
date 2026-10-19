#!/usr/bin/env python3
"""w2c_diag.py -- give wasm2c's call_indirect type check a fallback (toolchain, not game code).

wasm2c's generated `game-impl.h` traps on a signature mismatch with no information. This rewrites that ONE spot in the
generated header:

  * the slow path returns false on a TYPE mismatch instead of trapping (an out-of-bounds index or an empty table entry
    still trap, as before, after `te_call_indirect_bad` has said which index -- an unpatched retail address, usually);
  * `CALL_INDIRECT(table, t, ft, x, ...)` becomes `check ? direct call : te_adapt_##ft(&table, x, ...)` -- the call-site
    adapter of the caller's type (tools/gen_eabi_dispatch.py --adapt-header, one per module type), which packs the
    caller's arguments the EABI way and calls the body by the BODY's type. The runner counts and logs the adapted sites
    (te_adapt_note) so they stay visible; the vtable slot thunks (gen_vtable_thunks.py) remain the primary path.

Idempotent; fails loudly if the header no longer has the expected shape (a wabt upgrade).

    w2c_diag.py build/w2c/game-impl.h [--adapt-header eabi_adapt.h]
"""
import sys
from pathlib import Path

MARK = "// te: call_indirect type-adapter fallback (tools/w2c_diag.py)"
OLD_SLOW = """  if (index >= table->size) {
    // Table index out of bounds. Raise Wasm trap.
    TRAP(CALL_INDIRECT);
    return false;
  }
  const wasm_rt_funcref_t* const func_ref = &table->data[index];
  if (!expected_type || !func_ref->func || !func_ref->func_type ||
      memcmp(expected_type, func_ref->func_type, sha256size) != 0) {
    // Type check failed. Raise Wasm trap.
    TRAP(CALL_INDIRECT);
    return false;
  }"""
NEW_SLOW = """  if (index >= table->size) {
    // Table index out of bounds (an unpatched retail address, usually). Say which, then raise the Wasm trap.
    te_call_indirect_bad(table, expected_type, index);
    TRAP(CALL_INDIRECT);
    return false;
  }
  const wasm_rt_funcref_t* const func_ref = &table->data[index];
  if (!expected_type || !func_ref->func || !func_ref->func_type) {
    // An empty table entry. Say which, then raise the Wasm trap.
    te_call_indirect_bad(table, expected_type, index);
    TRAP(CALL_INDIRECT);
    return false;
  }
  if (memcmp(expected_type, func_ref->func_type, sha256size) != 0) {
    // Type check failed: the caller falls back to the type adapter of its declared signature (te_adapt_*)
    return false;
  }"""
ANCHOR = "W2C_COLD_FUNC static bool func_types_eq_slowpath("
DECL = "void te_call_indirect_bad(const wasm_rt_funcref_table_t* table, const char* expected_type, uint32_t index);\n"
OLD_MACRO = """#define CALL_INDIRECT(table, t, ft, x, ...) \\
  (CHECK_CALL_INDIRECT(table, ft, x),       \\
   DO_CALL_INDIRECT(table, t, x, __VA_ARGS__))"""
NEW_MACRO = """%s
#include "%s"
#define CALL_INDIRECT(table, t, ft, x, ...)                                      \\
  (CHECK_CALL_INDIRECT(table, ft, x) ? DO_CALL_INDIRECT(table, t, x, __VA_ARGS__) \\
                                     : te_adapt_##ft(&(table), x, __VA_ARGS__))"""


def main():
    args = sys.argv[1:]
    header = "eabi_adapt.h"
    if "--adapt-header" in args:
        i = args.index("--adapt-header")
        header = Path(args[i + 1]).name
        del args[i:i + 2]
    p = Path(args[0])
    s = p.read_text()
    if MARK in s:
        return 0
    if OLD_SLOW not in s or OLD_MACRO not in s or ANCHOR not in s:
        sys.stderr.write("w2c_diag: game-impl.h does not have the expected func_types_eq_slowpath / CALL_INDIRECT shape\n")
        return 1
    s = s.replace(OLD_SLOW, NEW_SLOW, 1).replace(OLD_MACRO, NEW_MACRO % (MARK, header), 1).replace(ANCHOR, DECL + ANCHOR, 1)
    p.write_text(s)
    print("w2c_diag: call_indirect type-adapter fallback added to", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
