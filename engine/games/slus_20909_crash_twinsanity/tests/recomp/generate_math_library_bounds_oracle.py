#!/usr/bin/env python3
"""Extract only the three reviewed NEW bounds bodies and their exact entries."""
import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = (
    ("Triangle", 0x201400, ()),
    ("Points", 0x201428, (0x20143C, 0x201440, 0x201454)),
    ("TransformedPoints", 0x201460, (0x2014A4, 0x2014A8, 0x2014CC)),
)
SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_017.cpp"
PREAMBLE = r'''// Actual NEW bounds bodies; no saved expected data or replacement arithmetic.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_bounds_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
namespace {
using Oracle = MathLibraryBoundsOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
std::array<uint64_t, 3> counts{};
namespace memory {
void read_quad(uint32_t address, QuadWord& value) {
    std::memcpy(&value, current_memory->Translate(address & ~15u, 16), 16);
}
void write_quad(uint32_t address, const QuadWord& value) {
    std::memcpy(current_memory->Translate(address & ~15u, 16), &value, 16);
}
}
'''


def generate(root, output):
    source = resolve(root, SOURCE).read_text()
    digest = hashlib.sha256()
    for relative in (SOURCE, "engine/ee/cpu/cpu_state.h", "engine/ee/recomp_abi/host_app/cpu_state.h",
                     "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    parts = [PREAMBLE]
    for i, (name, address, interiors) in enumerate(ENTRIES):
        body = extract(source, address, interiors)
        if re.search(r"\brecomp_call\b|\bg_vu[01]\b|\bResumeIfWaiting\b", code_only(body)):
            raise ValueError(f"{name}: unreviewed callee/model dependency")
        parts.append(f"void {name}(CpuContext& ctx, uint32_t addr) {{\n++counts[{i}];\n{body}\n}}\n")
    parts.append("}\n")
    parts.append("void MathLibraryBoundsOracle::Invoke(uint32_t entry, CpuContext& context, "
                 "Memory& memory, uint32_t dispatch) {\n"
                 "current_memory = &memory;\n"
                 "switch (entry) {\n")
    for name, address, _ in ENTRIES:
        parts.append(f"case 0x{address:x}: {name}(context, dispatch ? dispatch : entry); break;\n")
    parts.append("default: std::abort();\n}\ncurrent_memory = nullptr;\n}\n")
    parts.append("std::array<uint64_t, 3> MathLibraryBoundsOracle::Counts() { return counts; }\n")
    parts.append(f'const char* MathLibraryBoundsOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts))
    print("Extracted three NEW bounds bodies; source_sha256=" + digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.output)
        return 0
    except (OSError, ValueError) as error:
        print("Bounds oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
