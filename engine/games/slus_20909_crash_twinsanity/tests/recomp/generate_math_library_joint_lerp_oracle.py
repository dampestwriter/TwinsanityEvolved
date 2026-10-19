#!/usr/bin/env python3
"""Extract AnimateJoint's inline NEW macro scale blend (293ec0 block 294184) for a same-input oracle."""
import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_027.cpp"
ENTRY = 0x293EC0
INTERIORS = (
    0x293FA0, 0x293FB0, 0x294004, 0x29402C, 0x294054, 0x294068, 0x29407C, 0x294088,
    0x2940B0, 0x2940C0, 0x294114, 0x29413C, 0x294164, 0x294178, 0x294184, 0x2941C0,
    0x2941D0, 0x2941EC, 0x294208, 0x294214, 0x29422C, 0x29424C, 0x29425C, 0x29426C,
    0x2942CC)
BLOCK = "Label_0015: // 0x294184"
OPERATIONS = ("vaddax", "vmsubaw", "vmaddw", "vmove")
PREAMBLE = r'''// Actual NEW inline macro arithmetic of AnimateJoint 293ec0 block 294184; never replacement math.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_lerp_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
namespace {
'''


def generate(root, output):
    text = resolve(root, SOURCE).read_text()
    body = extract(text, ENTRY, INTERIORS)
    calls = re.findall(r"\bg_vu0\.Execute\s*\(([^)]*)\)", code_only(body))
    if calls != ["ctx, 0x818", "ctx, 0xa70", "ctx, 0x1c0"]:
        raise ValueError("AnimateJoint: unreviewed source program calls")
    helpers = re.findall(r"\brecomp_call\s*\(\s*(\d+)u\s*,\s*(0x[0-9a-f]+)u\s*,\s*ctx\s*\)", code_only(body))
    if helpers != [("7703", "0x297ba8"), ("7706", "0x297d88"), ("7707", "0x297e68")]:
        raise ValueError("AnimateJoint: unreviewed generated callees")
    start = body.index(BLOCK)
    first = body.index("        // vaddax", start)
    end = body.index("        // sqc2", first)
    fragment = body[first:end]
    found = re.findall(r"//\s*(v[a-z]+)\b", fragment)
    if found != list(OPERATIONS):
        raise ValueError(f"inline scale blend changed: {found}")
    refs = set(re.findall(r"\bctx\.(?:vuRegs\.(?:VF\[\d+\]|ACC)|cpuRegs[\w\[\].]*)", code_only(fragment)))
    if refs != {"ctx.vuRegs.VF[30]", "ctx.vuRegs.VF[31]", "ctx.vuRegs.VF[29]", "ctx.vuRegs.VF[0]",
                "ctx.vuRegs.ACC"}:
        raise ValueError(f"inline scale blend has unreviewed state references: {sorted(refs)}")
    if re.search(r"\bmemory::|\bg_vu0\b|\brecomp_call\b", code_only(fragment)):
        raise ValueError("inline scale blend fragment is not pure register arithmetic")
    digest = hashlib.sha256()
    for relative in (SOURCE, "engine/ee/cpu/cpu_state.h", "engine/ee/recomp_abi/host_app/cpu_state.h",
                     "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(relative.encode() + b"\0" + resolve(root, relative).read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(PREAMBLE + "void InlineScaleBlend(CpuContext& ctx) {\n" + fragment + "\n}\n}\n"
                      "void MathLibraryJointLerpOracle::Invoke(CpuContext& context) { InlineScaleBlend(context); }\n"
                      f'const char* MathLibraryJointLerpOracle::SourceDigest() {{ return "{digest.hexdigest()}"; }}\n')
    print("Extracted NEW inline scale blend; source_sha256=" + digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.root, args.output)
        return 0
    except (OSError, ValueError) as error:
        print("Joint lerp oracle generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
