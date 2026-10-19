#!/usr/bin/env python3
"""Extract the actual NEW RotateJoint source body and its reviewed interpolation call."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from generate_math_library_oracle import code_only, extract
from generate_math_library_joint_oracle import ELF_SHA256, image_from_elf
from recomp_paths import label, resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_027.cpp"
CALLER_INTERIORS = (
    0x2944CC, 0x2944D0, 0x2944D8, 0x2944F4, 0x294504, 0x294518, 0x294528, 0x294538,
    0x2945A8, 0x2945C8, 0x2945D8, 0x2945E8, 0x2945F0, 0x2945FC, 0x29460C, 0x29461C,
    0x29462C, 0x29463C, 0x29464C, 0x294654, 0x294664, 0x294674, 0x294684, 0x294694,
    0x2946A4, 0x294704, 0x29470C, 0x294710, 0x294718, 0x29471C)
PREAMBLE = r'''// Actual NEW RotateJoint body and NEW model, never replacement arithmetic.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_adapter_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include <cstdlib>
#include <cstring>
namespace {
using Oracle = MathLibraryJointAdapterOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
VuUnit* current_control = nullptr;
Oracle::Counters counters{};
class JointModel {
public:
    void Execute(CpuContext& context, uint32_t entry) {
        if (entry != 0x1c0) std::abort();
        std::memcpy(&current_control->regs(), &context.vuRegs, sizeof(VUregs));
        VuInterpreterTier model;
        ++counters.actual_interpolations;
        model.Run(*current_control, entry);
        std::memcpy(&context.vuRegs, &current_control->regs(), sizeof(VUregs));
    }
};
JointModel g_vu0;
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    auto* bytes = current_memory->Translate(address, sizeof(value));
    if (!bytes) std::abort();
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, T value) {
    auto* bytes = current_memory->Translate(address, sizeof(value));
    if (!bytes) std::abort();
    std::memcpy(bytes, &value, sizeof(value));
}
void read_quad(uint32_t address, QuadWord& value) { value = read<QuadWord>(address & ~15u); }
void write_quad(uint32_t address, const QuadWord& value) { write(address & ~15u, value); }
}
'''


def source_closure(root, elf):
    text = resolve(root, SOURCE).read_text()
    caller = code_only(extract(text, 0x294480, CALLER_INTERIORS))
    refs = set(re.findall(r"\bctx\.vuRegs\.[\w\[\].]+", caller))
    if refs != {"ctx.vuRegs.VF[1]"}:
        raise ValueError("DoAnimation has an unreviewed vector-state dependency")
    for address, interiors in ((0x297D88, (0x297D98, 0x297DF4)), (0x297E68, (0x297E78, 0x297ED4))):
        helper = code_only(extract(text, address, interiors))
        if re.search(r"\bvuRegs\b|\brecomp_call\b|\bg_vu[01]\b", helper):
            raise ValueError("translation/scale helper has an unreviewed vector-state dependency")
    callsites = []
    for path in sorted(resolve(root, SOURCE).parent.glob("recompiled_shard_*.cpp")):
        source = path.read_text()
        if "0x297b40u" not in source:
            continue
        for call in re.finditer(r"\brecomp_call\s*\(\s*(\d+)u\s*,\s*0x297b40u\s*,\s*ctx\s*\)", source):
            if call[1] != "7702":
                raise ValueError("RotateJoint generated dispatch identity changed")
            owners = list(re.finditer(r"\bvoid FUN_([0-9a-f]{8})\(", source[:call.start()]))
            owner = owners[-1][1] if owners else ""
            if owner != "00294480":
                raise ValueError("RotateJoint has an unreviewed direct caller")
            callsites.append({"source": label(path, root), "owner": owner,
                              "line": source[:call.start()].count("\n") + 1})
    if len(callsites) != 3:
        raise ValueError("RotateJoint direct caller count changed")
    data = elf.read_bytes()
    image_from_elf(data)  # Reviews ELF identity and exact original standard image.
    offset = struct.unpack_from("<I", data, 28)[0]
    stride, count = struct.unpack_from("<HH", data, 42)
    literals = []
    for i in range(count):
        kind, file_offset, virtual, _, size, *_ = struct.unpack_from("<8I", data, offset + i * stride)
        if kind != 1:
            continue
        for at in range((-virtual) % 4, size - 3, 4):
            if struct.unpack_from("<I", data, file_offset + at)[0] == 0x297B40:
                literals.append(virtual + at)
    if literals:
        raise ValueError("RotateJoint acquired an unreviewed aligned function-pointer candidate")
    return {"entry": "00297b40", "elf_sha256": ELF_SHA256, "direct_calls": callsites,
            "aligned_elf_pointer_candidates": literals, "caller_vector_refs": sorted(refs),
            "following_helpers": ["00297d88", "00297e68"],
            "contract": "Source result VF1 is stored to the animator rotation. Scalar helpers read no VU state.",
            "limits": "No whole-phase dynamic-pointer proof; AnimateJoint and downstream joint composition remain original."}


def generate(root, elf, output, manifest):
    closure = source_closure(root, elf)
    body = extract(resolve(root, SOURCE).read_text(), 0x297B40, (0x297B6C, 0x297B78, 0x297B90))
    code = code_only(body)
    calls = re.findall(r"\bg_vu0\.Execute\s*\(([^)]*)\)", code)
    if calls != ["ctx, 0x1c0"] or re.search(
            r"\brecomp_call\b|\bfind_containing_function\b|\bg_vu1\b|\bResumeIfWaiting\b", code):
        raise ValueError("RotateJoint: unreviewed source call dependency")
    digest = hashlib.sha256()
    for relative in (SOURCE, "engine/ee/cpu/cpu_state.h", "engine/ee/recomp_abi/host_app/cpu_state.h",
                     "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    tail = r'''
}
void MathLibraryJointAdapterOracle::Invoke(
        CpuContext& context, Memory& memory, VuUnit& control, uint32_t entry) {
    auto* prior_memory = current_memory;
    auto* prior_control = current_control;
    current_memory = &memory;
    current_control = &control;
    ++counters.generated_bodies;
    RotateJoint(context, entry);
    current_memory = prior_memory;
    current_control = prior_control;
}
MathLibraryJointAdapterOracle::Counters MathLibraryJointAdapterOracle::Counts() { return counters; }
'''
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(PREAMBLE + "void RotateJoint(CpuContext& ctx, uint32_t addr) {\n" + body +
                      "\n}\n" + tail +
                      f'const char* MathLibraryJointAdapterOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(closure, indent=2) + "\n")
    print("Extracted NEW RotateJoint source body; source_sha256=" + digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.elf, args.output, args.manifest)
        return 0
    except (OSError, ValueError) as error:
        print("Joint adapter oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
