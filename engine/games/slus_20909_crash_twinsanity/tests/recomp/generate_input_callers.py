"""Compile complete unchanged NEW controller callers against the Input service."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


CALLERS = {
    0x2B2558: {0x2C2BB8, 0x2C2C30, 0x2C2D68, 0x2C2DC8, 0x2C2EE8,
               0x2C3020, 0x2C31A0, 0x2C33D8, 0x2C3438},
    0x2B3890: {0x181590, 0x1815C0, 0x2C27C0, 0x2C6E88},
    0x2B3858: {0x181590, 0x2C29A8},
    0x2B3998: {0x2C30D8},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    relative = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_029.cpp")
    text = resolve(args.root, relative).read_text()
    bodies, evidence = [], []
    for address, expected in CALLERS.items():
        start = text.index(f"void FUN_{address:08x}(")
        body = text[start:text.index("\n// Function:", start)]
        calls = {int(value, 16) for value in re.findall(r"recomp_call\(\d+u, (0x[0-9a-f]+)u", body)}
        if calls != expected:
            raise RuntimeError(f"{address:#x} caller dependencies changed; review service closure")
        digest = hashlib.sha256(body.encode()).hexdigest()
        bodies.append(body)
        evidence.append({"entry": f"0x{address:08x}", "source": str(relative),
                         "source_sha256": digest, "compiled_sha256": digest,
                         "omitted": "", "calls": [f"0x{value:08x}" for value in sorted(calls)]})
    prefix = '''#include "games/slus_20909_crash_twinsanity/tests/recomp/input_caller_memory.h"
#include "engine/ee/cpu/ps2_float.h"
#include <algorithm>
namespace input_callers {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) { Dispatch(address, context); }
'''
    dispatch = ["bool Run(uint32_t address, CpuContext& context) { switch (address) {"]
    for address in CALLERS:
        dispatch.append(f"case 0x{address:x}: FUN_{address:08x}(context, address); return true;")
    dispatch += ["default: return false; } }", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "input_callers.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "input_callers.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print("Input callers: four complete unchanged NEW bodies, explicit callee closure")


if __name__ == "__main__":
    main()
