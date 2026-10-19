#!/usr/bin/env python3
"""Extract the actual NEW AnimateJoint source body, its three scalar callees and its reviewed program calls."""
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
ENTRY = 0x293EC0
INTERIORS = (
    0x293FA0, 0x293FB0, 0x294004, 0x29402C, 0x294054, 0x294068, 0x29407C, 0x294088,
    0x2940B0, 0x2940C0, 0x294114, 0x29413C, 0x294164, 0x294178, 0x294184, 0x2941C0,
    0x2941D0, 0x2941EC, 0x294208, 0x294214, 0x29422C, 0x29424C, 0x29425C, 0x29426C,
    0x2942CC)
# The angle decoder stays a separate counted boundary; the two pose blends are
# scalar bodies the adapter replaces as source operations. All three are
# extracted unchanged for the control arm and the decoder stand-in.
CALLEES = {
    0x297BA8: ("ReadJointAngle", (0x297BC0, 0x297BF8, 0x297C04, 0x297C0C, 0x297C54, 0x297C94)),
    0x297D88: ("ScaleJoint", (0x297D98, 0x297DF4)),
    0x297E68: ("TranslateJoint", (0x297E78, 0x297ED4)),
}
HELPERS = [("7703", "0x297ba8"), ("7706", "0x297d88"), ("7707", "0x297e68")]
PROGRAMS = ["ctx, 0x818", "ctx, 0xa70", "ctx, 0x1c0"]
# One reviewed direct caller (DoAnimation); the block after its call reads no
# vector state, so the animator object is the only consumed output.
DIRECT_CALLERS = {"00294480": 1}
DISPATCH_ID = "7657"
PREAMBLE = r'''// Actual NEW AnimateJoint body, its scalar callees and the NEW model; never replacement arithmetic.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_animate_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
using Oracle = MathLibraryJointAnimateOracle;
using QuadWord = GPR_reg;
Oracle::Memory* current_memory = nullptr;
VuUnit* current_control = nullptr;
Oracle::Counters counters{};
class JointModel {
public:
    void Execute(CpuContext& context, uint32_t entry) {
        if (entry == 0x818) ++counters.euler_programs;
        else if (entry == 0xa70) ++counters.turn_programs;
        else if (entry == 0x1c0) ++counters.interpolate_programs;
        else std::abort();
        if (!current_control) std::abort();
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
    if (!bytes) { std::fprintf(stderr, "actual body read of unmapped %08x\n", address); std::abort(); }
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, T value) {
    auto* bytes = current_memory->Translate(address, sizeof(value));
    if (!bytes) { std::fprintf(stderr, "actual body write of unmapped %08x\n", address); std::abort(); }
    std::memcpy(bytes, &value, sizeof(value));
}
void read_quad(uint32_t address, QuadWord& value) { value = read<QuadWord>(address & ~15u); }
void write_quad(uint32_t address, const QuadWord& value) { write(address & ~15u, value); }
}
void ReadJointAngle(CpuContext& ctx, uint32_t addr);
void ScaleJoint(CpuContext& ctx, uint32_t addr);
void TranslateJoint(CpuContext& ctx, uint32_t addr);
void recomp_call(uint32_t, uint32_t target, CpuContext& ctx) {
    if (target == 0x297ba8u) { ++counters.decoder_calls; ReadJointAngle(ctx, target); }
    else if (target == 0x297d88u) { ++counters.scale_calls; ScaleJoint(ctx, target); }
    else if (target == 0x297e68u) { ++counters.translate_calls; TranslateJoint(ctx, target); }
    else std::abort();
}
'''


def owner_body(masked, start):
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
                raise ValueError("AnimateJoint generated dispatch identity changed")
            owner, _ = owner_body(masked, call.start())
            following = re.search(r"\bLabel_\d+\s*:", masked[call.end():])
            block = masked[call.end():call.end() + following.start()] if following else masked[call.end():]
            if re.search(r"\bvuRegs\b", block):
                raise ValueError(f"caller {owner} reads vector state right after AnimateJoint")
            owners[owner] = owners.get(owner, 0) + 1
            callsites.append({"source": label(path, root), "owner": owner,
                              "line": source[:call.start()].count("\n") + 1})
    if owners != DIRECT_CALLERS:
        raise ValueError(f"AnimateJoint direct caller census changed: {owners}")
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
        raise ValueError("AnimateJoint acquired an unreviewed aligned function-pointer candidate")
    return {"entry": f"{ENTRY:08x}", "elf_sha256": ELF_SHA256, "direct_calls": callsites,
            "direct_caller_bodies": owners, "aligned_elf_pointer_candidates": literals,
            "callees": {f"{address:08x}": name for address, (name, _) in CALLEES.items()},
            "contract": "DoAnimation consumes the animator object (flags, rotation, translation, scale) "
                        "written by the call and continues its joint loop; no vector or temporary register "
                        "is read after the call.",
            "limits": "Static direct-call census only; the angle decoder keeps its own boundary and the "
                      "two pose blends remain original for their other callers."}


def generate(root, elf, output, manifest):
    closure = source_closure(root, elf)
    text = resolve(root, SOURCE).read_text()
    body = extract(text, ENTRY, INTERIORS)
    code = code_only(body)
    calls = re.findall(r"\bg_vu0\.Execute\s*\(([^)]*)\)", code)
    helpers = re.findall(r"\brecomp_call\s*\(\s*(\d+)u\s*,\s*(0x[0-9a-f]+)u\s*,\s*ctx\s*\)", code)
    if calls != PROGRAMS or helpers != HELPERS or re.search(
            r"\bfind_containing_function\b|\bg_vu1\b|\bResumeIfWaiting\b", code):
        raise ValueError("AnimateJoint: unreviewed source call dependency")
    callees = []
    for address, (name, interiors) in CALLEES.items():
        callee = extract(text, address, interiors)
        if re.search(r"\brecomp_call\b|\bg_vu0\b|\bg_vu1\b|\bfind_containing_function\b", code_only(callee)):
            raise ValueError(f"{name}: unreviewed callee dependency")
        callees.append(f"void {name}(CpuContext& ctx, uint32_t addr) {{\n{callee}\n}}\n")
    digest = hashlib.sha256()
    for relative in (SOURCE, "engine/ee/cpu/cpu_state.h", "engine/ee/recomp_abi/host_app/cpu_state.h",
                     "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    tail = r'''
}
void MathLibraryJointAnimateOracle::Invoke(
        CpuContext& context, Memory& memory, VuUnit& control, uint32_t entry) {
    auto* prior_memory = current_memory;
    auto* prior_control = current_control;
    current_memory = &memory;
    current_control = &control;
    ++counters.generated_bodies;
    AnimateJoint(context, entry);
    current_memory = prior_memory;
    current_control = prior_control;
}
void MathLibraryJointAnimateOracle::InvokeDecoder(CpuContext& context, Memory& memory, uint32_t entry) {
    auto* prior_memory = current_memory;
    current_memory = &memory;
    ++counters.decoder_calls;
    ReadJointAngle(context, entry);
    current_memory = prior_memory;
}
MathLibraryJointAnimateOracle::Counters MathLibraryJointAnimateOracle::Counts() { return counters; }
'''
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(PREAMBLE + "".join(callees) +
                      "void AnimateJoint(CpuContext& ctx, uint32_t addr) {\n" + body + "\n}\n" + tail +
                      f'const char* MathLibraryJointAnimateOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(closure, indent=2) + "\n")
    print("Extracted NEW AnimateJoint source body and callees; source_sha256=" + digest.hexdigest())


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
        print("Joint animate oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
