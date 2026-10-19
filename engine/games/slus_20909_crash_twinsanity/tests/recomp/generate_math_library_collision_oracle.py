#!/usr/bin/env python3
"""Extract NEW's eight collision boundaries; share the math extractor's checks."""

import argparse
import hashlib
from pathlib import Path
import sys

from generate_math_library_oracle import extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_027.cpp"
ENTRIES = (
    ("StartTriangleEdgeTests", 0x292AE8, ()),
    ("FinishTriangleEdgeTests", 0x292B8C, ()),
    ("MakeVertexDifferences", 0x292C04,
     (0x292C48, 0x292C70, 0x292C78, 0x292CA0, 0x292CA8, 0x292CB4, 0x292CD0, 0x292CE0)),
    ("GroupPoints", 0x292CE8, (0x292CF4, 0x292D3C)),
    ("GroupsRange", 0x292D44, (0x292D74, 0x292DA0)),
    ("SixteenGroupsRange", 0x292DCC, (0x292E08, 0x292E54)),
    ("LoadTriangleHullSupport", 0x292E80, ()),
    ("TriangleHullSupportRange", 0x293064, ()),
)

PREAMBLE = r'''// Generated from NEW; no OLD, ELF, captures or static expected data.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_collision_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
using QuadWord = GPR_reg;
MathLibraryCollisionOracle::Memory* current_memory = nullptr;
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value, current_memory->Translate(address, sizeof(value)), sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, T value) {
    std::memcpy(current_memory->Translate(address, sizeof(value)), &value, sizeof(value));
}
void read_quad(uint32_t address, QuadWord& value) {
    std::memcpy(&value, current_memory->Translate(address & ~15u, 16), 16);
}
void write_quad(uint32_t address, const QuadWord& value) {
    std::memcpy(current_memory->Translate(address & ~15u, 16), &value, 16);
}
}
'''


def generate(root, output):
    data = resolve(root, SOURCE).read_bytes()
    text = data.decode("utf-8")
    parts = [PREAMBLE]
    for name, address, interiors in ENTRIES:
        body = extract(text, address, interiors)
        parts.append(f"\nvoid Oracle{name}(CpuContext& ctx, uint32_t addr) {{\n{body}\n}}\n")
    parts.append("}  // namespace\n")
    parts.append("const std::array<MathLibraryCollisionOracle::Entry, MathLibraryCollisionOracle::kEntryCount>&\n"
                 "MathLibraryCollisionOracle::Entries() {\n"
                 "    static const std::array<Entry, kEntryCount> entries{{\n")
    parts.extend(f'        {{"{name}", 0x{address:08x}u}},\n' for name, address, _ in ENTRIES)
    parts.append("    }};\n    return entries;\n}\n")
    parts.append("const char* MathLibraryCollisionOracle::SourceDigest() {\n"
                 f'    return "{hashlib.sha256(data).hexdigest()}";\n}}\n')
    parts.append("void MathLibraryCollisionOracle::Invoke(uint32_t address, CpuContext& context, Memory& memory) {\n"
                 "    auto* previous = current_memory;\n"
                 "    current_memory = &memory;\n"
                 "    switch (address) {\n")
    parts.extend(f"        case 0x{address:08x}u: Oracle{name}(context, address); break;\n"
                 for name, address, _ in ENTRIES)
    parts.append("        default: std::abort();\n    }\n    current_memory = previous;\n}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts), encoding="utf-8")
    print(f"Extracted {len(ENTRIES)} NEW collision bodies; sha256={hashlib.sha256(data).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        generate(arguments.root, arguments.output)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Collision oracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
