#!/usr/bin/env python3
"""Extract the actual NEW CreateJointTransform source body and its reviewed joint program calls."""
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
ENTRY = 0x294300
INTERIORS = (
    0x294318, 0x294324, 0x29432C, 0x294334, 0x294340, 0x294344, 0x294358, 0x294368,
    0x294374, 0x29438C, 0x294398, 0x2943A4, 0x2943AC, 0x2943E0, 0x2943E8, 0x2943F8,
    0x29440C, 0x294418, 0x29441C, 0x294424, 0x29442C, 0x294454, 0x29445C, 0x294464,
    0x294478)
# Nine reviewed direct calls in seven generated owners; every owner body has no
# direct vector-state reference, so the complete joint matrix is its only output.
DIRECT_CALLERS = {"00116170": 1, "00148148": 1, "0014b860": 3, "0014c7d8": 1,
                  "0018fc50": 1, "002369a8": 1, "00294b18": 1}
DISPATCH_ID = "7658"
PREAMBLE = r'''// Actual NEW CreateJointTransform body and NEW model, never replacement arithmetic.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_transform_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include <cstdlib>
#include <cstring>
namespace {
using Oracle = MathLibraryJointTransformOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
VuUnit* current_control = nullptr;
Oracle::Counters counters{};
class JointModel {
public:
    void Execute(CpuContext& context, uint32_t entry) {
        if (entry == 0x548) ++counters.actual_bases;
        else if (entry == 0x668) ++counters.actual_scaled_bases;
        else if (entry == 0x790) ++counters.actual_parent_products;
        else std::abort();
        std::memcpy(&current_control->regs(), &context.vuRegs, sizeof(VUregs));
        VuInterpreterTier model;
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


def owner_body(masked, start):
    """Masked text of the generated function containing offset start."""
    owners = list(re.finditer(r"\bvoid FUN_([0-9a-f]{8})\(", masked[:start]))
    if not owners:
        raise ValueError("call outside a generated function")
    begin = owners[-1].end()
    following = re.search(r"\bvoid FUN_[0-9a-f]{8}\(", masked[begin:])
    end = begin + following.start() if following else len(masked)
    return owners[-1][1], masked[begin:end]


def source_closure(root, elf):
    callsites = []
    owners = {}
    for path in sorted(resolve(root, SOURCE).parent.glob("recompiled_shard_*.cpp")):
        source = path.read_text()
        if f"0x{ENTRY:x}u" not in source:
            continue
        masked = code_only(source)
        for call in re.finditer(rf"\brecomp_call\s*\(\s*(\d+)u\s*,\s*0x{ENTRY:x}u\s*,\s*ctx\s*\)", masked):
            if call[1] != DISPATCH_ID:
                raise ValueError("CreateJointTransform generated dispatch identity changed")
            owner, body = owner_body(masked, call.start())
            if re.search(r"\bvuRegs\b", body):
                raise ValueError(f"caller {owner} has an unreviewed direct vector-state dependency")
            owners[owner] = owners.get(owner, 0) + 1
            callsites.append({"source": label(path, root), "owner": owner,
                              "line": source[:call.start()].count("\n") + 1})
    if owners != DIRECT_CALLERS:
        raise ValueError(f"CreateJointTransform direct caller census changed: {owners}")
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
            if struct.unpack_from("<I", data, file_offset + at)[0] == ENTRY:
                literals.append(virtual + at)
    if literals:
        raise ValueError("CreateJointTransform acquired an unreviewed aligned function-pointer candidate")
    return {"entry": f"{ENTRY:08x}", "elf_sha256": ELF_SHA256, "direct_calls": callsites,
            "direct_caller_bodies": owners, "aligned_elf_pointer_candidates": literals,
            "contract": "Callers consume the stored output matrix, the published animation scale/bits and the "
                        "animator done flag. No caller reads vector state after the call.",
            "limits": "Static direct-call census only; indirect/dynamic pointers, AnimateJoint and the graphics "
                      "parent-composition consumers remain original."}


def generate(root, elf, output, manifest):
    closure = source_closure(root, elf)
    body = extract(resolve(root, SOURCE).read_text(), ENTRY, INTERIORS)
    code = code_only(body)
    calls = re.findall(r"\bg_vu0\.Execute\s*\(([^)]*)\)", code)
    if calls != ["ctx, 0x668", "ctx, 0x548", "ctx, 0x790"] or re.search(
            r"\brecomp_call\b|\bfind_containing_function\b|\bg_vu1\b|\bResumeIfWaiting\b", code):
        raise ValueError("CreateJointTransform: unreviewed source call dependency")
    digest = hashlib.sha256()
    for relative in (SOURCE, "engine/ee/cpu/cpu_state.h", "engine/ee/recomp_abi/host_app/cpu_state.h",
                     "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    tail = r'''
}
void MathLibraryJointTransformOracle::Invoke(
        CpuContext& context, Memory& memory, VuUnit& control, uint32_t entry) {
    auto* prior_memory = current_memory;
    auto* prior_control = current_control;
    current_memory = &memory;
    current_control = &control;
    ++counters.generated_bodies;
    CreateJointTransform(context, entry);
    current_memory = prior_memory;
    current_control = prior_control;
}
MathLibraryJointTransformOracle::Counters MathLibraryJointTransformOracle::Counts() { return counters; }
'''
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(PREAMBLE + "void CreateJointTransform(CpuContext& ctx, uint32_t addr) {\n" + body +
                      "\n}\n" + tail +
                      f'const char* MathLibraryJointTransformOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(closure, indent=2) + "\n")
    print("Extracted NEW CreateJointTransform source body; source_sha256=" + digest.hexdigest())


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
        print("Joint transform oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
