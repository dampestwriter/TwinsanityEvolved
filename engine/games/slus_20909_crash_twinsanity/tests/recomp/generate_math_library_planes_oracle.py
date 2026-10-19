#!/usr/bin/env python3
"""Extract NEW plane preparation and every generated arithmetic dependency."""

import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


# name, shard, NTSC address, reviewed interior entries, exact child dispatches.
ENTRIES = (
    ("RotateVector", 9, 0x18E4B8, (), ()),
    ("TransformPlane", 9, 0x18D310, (0x18D334,), ((2419, 0x18E4B8),)),
    ("TransformPlaneInPlace", 9, 0x18D388, (0x18D3B0,), ((2419, 0x18E4B8),)),
    ("TransformPlanes", 17, 0x200E18, (0x200E38, 0x200E44, 0x200E54), ((2386, 0x18D388),)),
    ("SetPlaneColumn", 17, 0x1FD760, (0x1FD7C8, 0x1FD7EC), ()),
    ("SetSidePlaneColumns", 17, 0x200B08, (0x200B30, 0x200B44, 0x200B54), ((4572, 0x1FD760),)),
    ("ViewPlanesIn", 15, 0x1EED10,
     (0x1EED40, 0x1EED58, 0x1EED68, 0x1EED80, 0x1EED8C, 0x1EED98),
     ((4684, 0x200E18), (4679, 0x200B08))),
    ("FarViewPlanesIn", 15, 0x1EEDC0,
     (0x1EEDE8, 0x1EEE00, 0x1EEE10, 0x1EEE28, 0x1EEE34, 0x1EEE40),
     ((4684, 0x200E18), (4679, 0x200B08))),
)

PREAMBLE = r'''// Extracted actual NEW bodies; no copied expectations or OLD code.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_planes_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
namespace {
using QuadWord = GPR_reg;
MathLibraryPlanesOracle::Memory* current_memory = nullptr;
std::array<uint64_t, 8> counts{};
std::ofstream g_logFile;
namespace memory {
template<class T> T read(uint32_t address) {
    T result;
    std::memcpy(&result, current_memory->Translate(address, sizeof(result)), sizeof(result));
    return result;
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
void recomp_call(uint32_t id, uint32_t address, CpuContext& context);
'''


def generate(root, output):
    sources, digest = {}, hashlib.sha256()
    for shard in (9, 15, 17):
        relative = f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03d}.cpp"
        data = resolve(root, relative).read_bytes()
        sources[shard] = data.decode()
        digest.update(relative.encode() + b"\0" + data)
    for relative in ("engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl",
                     "engine/ee/recomp_abi/host_app/cpu_state.h"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    parts = [PREAMBLE]
    for index, (name, shard, address, interiors, calls) in enumerate(ENTRIES):
        body = extract(sources[shard], address, interiors)
        code = code_only(body)
        observed = re.findall(r"\brecomp_call\s*\(\s*(\d+)u,\s*(0x[0-9a-f]+)u,\s*ctx\s*\)", code)
        if tuple((int(i), int(a, 16)) for i, a in observed) != calls:
            raise ValueError(f"{name}: generated child closure changed")
        if re.search(r"\bg_vu[01]\b|\bResumeIfWaiting\b", code):
            raise ValueError(f"{name}: unexpected hardware dependency")
        parts.append(f"void {name}(CpuContext& ctx, uint32_t addr) {{\n"
                     f"++counts[{index}];\n{body}\n}}\n")
    parts.append("void recomp_call(uint32_t id, uint32_t address, CpuContext& context) {\n")
    known = {pair for *_, calls in ENTRIES for pair in calls}
    names = {address: name for name, _, address, _, _ in ENTRIES}
    for function_id, address in sorted(known):
        parts.append(f"if (id == {function_id}u && address == 0x{address:x}u) "
                     f"{{ {names[address]}(context, address); return; }}\n")
    parts.append("std::abort();\n}\n} // namespace\n")
    parts.append("std::array<uint64_t, 8> MathLibraryPlanesOracle::Counts() { return counts; }\n"
                 f'const char* MathLibraryPlanesOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n'
                 "void MathLibraryPlanesOracle::Invoke(uint32_t address, CpuContext& context, Memory& memory) {\n"
                 "auto* previous = current_memory;\ncurrent_memory = &memory;\ntry {\nswitch (address) {\n")
    for name, _, address, _, _ in ENTRIES:
        parts.append(f"case 0x{address:x}u: {name}(context, address); break;\n")
    parts.append("default: std::abort();\n}\n} catch (...) { current_memory = previous; throw; }\n"
                 "current_memory = previous;\n}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts))
    print(f"Extracted {len(ENTRIES)} actual NEW plane bodies; sha256={digest.hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.output)
    except (OSError, ValueError) as error:
        print(f"Plane oracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
