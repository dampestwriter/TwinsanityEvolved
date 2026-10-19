#!/usr/bin/env python3
"""Extract NEW's source animation angle decoder; no recorded expected answers."""

import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_027.cpp"
ADDRESS = 0x297BA8
INTERIORS = (0x297BC0, 0x297BF8, 0x297C04, 0x297C0C, 0x297C54, 0x297C94)

PREAMBLE = r'''// Extracted from NEW; no OLD code, ELF or capture fixtures.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_animation_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <cstring>
namespace {
MathLibraryAnimationOracle::Memory* current_memory = nullptr;
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value, current_memory->Translate(address, sizeof(value)), sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, T value) {
    std::memcpy(current_memory->Translate(address, sizeof(value)), &value, sizeof(value));
}
}
'''


def generate(root, output):
    data = resolve(root, SOURCE).read_bytes()
    body = extract(data.decode("utf-8"), ADDRESS, INTERIORS)
    if re.search(r"\brecomp_call\s*\(|\bg_vu[01]\b", code_only(body)):
        raise ValueError("unexpected decoder dependency; review operation closure")
    # Pin the historical signed-immediate correction without baking in answers.
    if "static_cast<int64_t>(-0x8000)" not in body:
        raise ValueError("NEW's signed half-turn comparison changed; review historical wrap fix")
    text = PREAMBLE + f"\nvoid Decode(CpuContext& ctx, uint32_t addr) {{\n{body}\n}}\n"
    text += "}  // namespace\n"
    text += "const char* MathLibraryAnimationOracle::SourceDigest() {\n"
    text += f'    return "{hashlib.sha256(data).hexdigest()}";\n}}\n'
    text += "void MathLibraryAnimationOracle::Invoke(CpuContext& context, Memory& memory) {\n"
    text += "    auto* previous = current_memory;\n    current_memory = &memory;\n"
    text += f"    Decode(context, 0x{ADDRESS:08x}u);\n    current_memory = previous;\n}}\n"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    print(f"Extracted NEW animation angle decoder; sha256={hashlib.sha256(data).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        generate(arguments.root, arguments.output)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Animation oracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
