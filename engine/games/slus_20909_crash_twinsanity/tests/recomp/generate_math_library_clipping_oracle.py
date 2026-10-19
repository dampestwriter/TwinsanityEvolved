#!/usr/bin/env python3
"""Extract NEW box/corner clipping bodies, including their exact source tail call."""
import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = (("BoxAt", 0x2014D8, ()), ("Box", 0x20157C, (0x2015D0,)))
SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_017.cpp"
PREAMBLE = r'''// Actual NEW bodies, not stored expected outputs or replacement math.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_clipping_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
namespace {
using Oracle = MathLibraryClippingOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
const Oracle::Body* current_corners = nullptr;
std::array<uint64_t, 2> counts{};
std::ofstream g_logFile;
void Box(CpuContext&, uint32_t);
const Oracle::Body* find_containing_function(uint32_t address) {
    if (address != 0x2015d0) std::abort();
    static const Oracle::Body original = Box;
    return current_corners ? current_corners : &original;
}
void recomp_indirect_call(const Oracle::Body* body, uint32_t address, CpuContext& context) {
    if (!body || address != 0x2015d0) std::abort();
    (*body)(context, address);
}
namespace memory {
void read_quad(uint32_t address, QuadWord& value) {
    std::memcpy(&value, current_memory->Translate(address & ~15u, 16), 16);
}
template<class T> void write(uint32_t address, T value) {
    std::memcpy(current_memory->Translate(address, sizeof(value)), &value, sizeof(value));
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
        code = code_only(body)
        if re.search(r"\brecomp_call\b|\bg_vu[01]\b|\bResumeIfWaiting\b", code):
            raise ValueError(f"{name}: unreviewed callee/model dependency")
        tails = re.findall(r"\bfind_containing_function\s*\(\s*(0x[0-9a-f]+)\s*\)", code)
        calls = re.findall(r"\brecomp_indirect_call\s*\(([^)]*)\)", code)
        if tails != (["0x2015d0"] if i == 0 else []) or calls != (
                ["func_ptr, 0x2015d0, ctx"] if i == 0 else []):
            raise ValueError(f"{name}: source tail call changed")
        parts.append(f"void {name}(CpuContext& ctx, uint32_t addr) {{\n++counts[{i}];\n{body}\n}}\n")
    parts.append("}\n")
    parts.append(r'''void MathLibraryClippingOracle::Invoke(
        uint32_t entry, CpuContext& context, Memory& memory, uint32_t dispatch, const Body* corners) {
    auto* prior_memory = current_memory;
    auto* prior_corners = current_corners;
    current_memory = &memory;
    current_corners = corners;
    try {
        switch (entry) {
        case 0x2014d8: BoxAt(context, dispatch ? dispatch : entry); break;
        case 0x20157c: Box(context, dispatch ? dispatch : entry); break;
        default: std::abort();
        }
    } catch (...) {
        current_memory = prior_memory;
        current_corners = prior_corners;
        throw;
    }
    current_memory = prior_memory;
    current_corners = prior_corners;
}
std::array<uint64_t, 2> MathLibraryClippingOracle::Counts() { return counts; }
''')
    parts.append(f'const char* MathLibraryClippingOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts))
    print("Extracted two NEW clipping bodies and source ClipCorners tail; source_sha256=" + digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.output)
        return 0
    except (OSError, ValueError) as error:
        print("Clipping oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
