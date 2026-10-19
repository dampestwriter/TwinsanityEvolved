#!/usr/bin/env python3
"""Extract NEW source culling boundaries with closed arithmetic dependencies."""

import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from generate_math_library_planes_oracle import ENTRIES as PLANE_ENTRIES
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


# Tests expose the two transport calls as request sinks. They are not counted
# as original body execution, and do not establish transport-model retirement.
TRANSPORT = ((8133, 0x2B1860), (8134, 0x2B18F8))
ENTRIES = PLANE_ENTRIES + (
    ("Identity", 9, 0x18E570, (), ()),
    ("MultiplyMatrices", 9, 0x18E5A0, (), ()),
    ("InvertRigid", 9, 0x18E788, (), ()),
    ("Packet", 16, 0x1F3810, (0x1F3820, 0x1F3838), ()),
    ("AddColumns", 17, 0x200B70, (0x200B78, 0x200BA8), ()),
    ("ClearColumns", 17, 0x200BB0, (0x200BB8, 0x200BD0), ()),
    ("TransformPlaneOf", 17, 0x200E70, (0x200E8C,), ((2385, 0x18D310),)),
    ("CopyVector", 1, 0x11C098, (), ()),
    ("TakeViewMatrix", 16, 0x1F5F18, (), ()),
    ("ResetView", 16, 0x1F5C20, (0x1F5C3C, 0x1F5C48, 0x1F5C6C, 0x1F5C74, 0x1F5C90),
     ((4506, 0x1F5F18), (185, 0x11C098))),
    ("SetUpChunk", 15, 0x1E8778,
     (0x1E87E8, 0x1E8800, 0x1E8818, 0x1E8830, 0x1E8840, 0x1E8848, 0x1E8854,
      0x1E8860, 0x1E886C, 0x1E8874, 0x1E8884, 0x1E8894, 0x1E88A0, 0x1E88B8, 0x1E88D0, 0x1E88DC),
     ((4225, 0x1EED10), (4416, 0x1F3810), (4680, 0x200B70), (4226, 0x1EEDC0),
      (4680, 0x200B70), (4681, 0x200BB0), (4572, 0x1FD760), (4572, 0x1FD760),
      (4680, 0x200B70), (8134, 0x2B18F8), (2423, 0x18E5A0), (2428, 0x18E788))),
    ("SetUpPortal", 15, 0x1E8910,
     (0x1E8978, 0x1E8990, 0x1E899C, 0x1E89A4, 0x1E89B0, 0x1E89BC, 0x1E89C8,
      0x1E89D0, 0x1E89E8, 0x1E89F8, 0x1E8A04, 0x1E8A1C, 0x1E8A34, 0x1E8A40),
     ((4679, 0x200B08), (4416, 0x1F3810), (4680, 0x200B70), (4226, 0x1EEDC0),
      (4680, 0x200B70), (4681, 0x200BB0), (4685, 0x200E70), (4572, 0x1FD760),
      (4680, 0x200B70), (8134, 0x2B18F8), (2423, 0x18E5A0), (2428, 0x18E788))),
    ("LoadView", 16, 0x1F4E50, (0x1F4E6C, 0x1F4F68, 0x1F4F78, 0x1F4F80),
     ((4416, 0x1F3810), (8134, 0x2B18F8), (8133, 0x2B1860), (2422, 0x18E570))),
    ("ConstructLinked", 16, 0x1F4FB8, (0x1F4FF4,), ((4496, 0x1F5C20),)),
    ("LoadModel", 16, 0x1F5F70, (), ()),
    ("TestCell", 16, 0x1F5F90, (), ()),
    ("TestBoxAt", 16, 0x1F5FE8, (), ()),
    ("CellResult", 16, 0x1F5FB0, (0x1F5FC4, 0x1F5FCC, 0x1F5FD4, 0x1F5FD8, 0x1F5FDC), ()),
    ("DrawnModelResult", 16, 0x1F5F48, (), ()),
    ("MeshResult", 16, 0x1F6028, (0x1F6044, 0x1F6050, 0x1F6058, 0x1F607C, 0x1F608C, 0x1F609C), ()),
    ("InstanceResult", 16, 0x1F60A8, (0x1F60C4, 0x1F60D0, 0x1F60D8, 0x1F60DC, 0x1F60E4), ()),
)

PREAMBLE = r'''// Actual NEW bodies; transport request sinks are explicit test seams.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_culling_adapter_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
namespace {
using Oracle = MathLibraryCullingAdapterOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
VuUnit* current_control = nullptr;
Oracle::Counters counters{};
std::ofstream g_logFile;
class CullingModel {
public:
    void Execute(CpuContext& context, uint32_t entry) {
        if (entry != 0x398 && entry != 0x2d0 && entry != 0x1c0 && entry != 0x500) std::abort();
        std::memcpy(&current_control->regs(), &context.vuRegs, sizeof(VUregs));
        VuInterpreterTier model;
        ++counters.actual_culling_executions;
        model.Run(*current_control, entry);
        std::memcpy(&context.vuRegs, &current_control->regs(), sizeof(VUregs));
    }
};
CullingModel g_vu0;
namespace memory {
template<class T> T read(uint32_t address) {
    T result;
    auto* data = current_memory->Translate(address, sizeof(result));
    if (!data) std::abort();
    std::memcpy(&result, data, sizeof(result));
    return result;
}
template<class T> void write(uint32_t address, T value) {
    auto* data = current_memory->Translate(address, sizeof(value));
    if (!data) std::abort();
    std::memcpy(data, &value, sizeof(value));
}
void read_quad(uint32_t address, QuadWord& value) {
    auto* data = current_memory->Translate(address & ~15u, 16);
    if (!data) std::abort();
    std::memcpy(&value, data, 16);
}
void write_quad(uint32_t address, const QuadWord& value) {
    auto* data = current_memory->Translate(address & ~15u, 16);
    if (!data) std::abort();
    std::memcpy(data, &value, 16);
}
}
void recomp_call(uint32_t id, uint32_t address, CpuContext& context);
'''


def generate(root, output):
    sources, digest = {}, hashlib.sha256()
    for shard in sorted({entry[1] for entry in ENTRIES}):
        relative = f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03d}.cpp"
        data = resolve(root, relative).read_bytes()
        sources[shard] = data.decode()
        digest.update(relative.encode() + b"\0" + data)
    parts = [PREAMBLE]
    for name, shard, address, interiors, calls in ENTRIES:
        body = extract(sources[shard], address, interiors)
        code = code_only(body)
        observed = re.findall(r"\brecomp_call\s*\(\s*(\d+)u,\s*(0x[0-9a-f]+)u,\s*ctx\s*\)", code)
        if tuple((int(i), int(a, 16)) for i, a in observed) != calls:
            raise ValueError(f"{name}: generated child closure changed")
        expected_model = {
            "LoadView": [0x398], "ConstructLinked": [0x398], "LoadModel": [0x2D0],
            "TestCell": [0x1C0], "TestBoxAt": [0x500]}.get(name, [])
        actual_model = re.findall(r"\bg_vu0\.Execute\s*\(\s*ctx,\s*(0x[0-9a-f]+)\s*\)", code)
        if list(map(lambda a: int(a, 16), actual_model)) != expected_model:
            raise ValueError(f"{name}: culling arithmetic calls changed")
        if re.search(r"\bResumeIfWaiting\b|\bg_vu1\b", code):
            raise ValueError(f"{name}: unsupported model dependency")
        parts.append(f"void {name}(CpuContext& ctx, uint32_t addr) {{\n"
                     f"++counters.generated_bodies[0x{address:x}u];\n{body}\n}}\n")
    parts.append("void recomp_call(uint32_t id, uint32_t address, CpuContext& context) {\n")
    names = {address: name for name, _, address, _, _ in ENTRIES}
    for function_id, address in sorted({pair for *_, calls in ENTRIES for pair in calls}):
        if (function_id, address) in TRANSPORT:
            continue
        parts.append(f"if (id == {function_id}u && address == 0x{address:x}u) "
                     f"{{ {names[address]}(context, address); return; }}\n")
    parts.append(r'''
if (id == 8134u && address == 0x2b18f8u) {
    const auto source = context.cpuRegs.GPR.r[5].UL[0];
    const auto rows = context.cpuRegs.GPR.r[6].UL[0];
    const auto destination = context.cpuRegs.GPR.r[7].UL[0];
    if (!((rows == 21 && destination == 0x80) || (rows == 9 && destination == 0x98))) std::abort();
    auto* data = current_memory->Translate(source, rows * 16u);
    if (!data) std::abort();
    ++counters.upload_requests;
    current_control->WriteDataMem(destination * 16u, data, rows * 16u);
    return;
}
if (id == 8133u && address == 0x2b1860u) {
    if (context.cpuRegs.GPR.r[5].UL[0] != 2 || context.cpuRegs.GPR.r[6].UL[0] != 1) std::abort();
    ++counters.selection_requests;
    return;
}
std::abort();
}
} // namespace
MathLibraryCullingAdapterOracle::Counters MathLibraryCullingAdapterOracle::Counts() { return counters; }
''')
    parts.append(f'const char* MathLibraryCullingAdapterOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n'
                 "void MathLibraryCullingAdapterOracle::Invoke(uint32_t address, CpuContext& context,\n"
                 "                                            Memory& memory, VuUnit& control) {\n"
                 "auto* previous = current_memory; auto* previous_control = current_control;\n"
                 "current_memory = &memory; current_control = &control;\ntry {\nswitch (address) {\n")
    for name, _, address, _, _ in ENTRIES:
        parts.append(f"case 0x{address:x}u: {name}(context, address); break;\n")
    parts.append("default: std::abort();\n}\n"
                 "} catch (...) { current_memory = previous; current_control = previous_control; throw; }\n"
                 "current_memory = previous; current_control = previous_control;\n}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts))
    print(f"Extracted {len(ENTRIES)} actual NEW culling bodies; sha256={digest.hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.output)
    except (OSError, ValueError) as error:
        print(f"Culling adapter oracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
