"""Compile unchanged NEW save Request/Poll/Update/device-presence callers."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


CALLERS = {
    0x2A2430: {0x2A8638},
    0x2A2548: {0x2A69F0, 0x2A88C0, 0x2A8728, 0x2A8A60, 0x2A8C28, 0x2A8DF0},
    0x2A8558: {0x2A6B68},
    0x2A8510: set(),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    relative = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_028.cpp")
    text = resolve(args.root, relative).read_text()
    bodies, evidence = [], []
    for address, expected in CALLERS.items():
        start = text.index(f"void FUN_{address:08x}(")
        body = text[start:text.index("\n// Function:", start)]
        calls = {int(value, 16) for value in re.findall(r"recomp_call\(\d+u, (0x[0-9a-f]+)u", body)}
        if calls != expected:
            raise RuntimeError(f"{address:#x} save dependencies changed; review caller closure")
        digest = hashlib.sha256(body.encode()).hexdigest()
        bodies.append(body)
        evidence.append({"entry": f"0x{address:08x}", "source": str(relative),
                         "source_sha256": digest, "compiled_sha256": digest,
                         "omitted": "", "calls": [f"0x{value:08x}" for value in sorted(calls)]})
    prefix = '''#include "games/slus_20909_crash_twinsanity/tests/recomp/save_caller_memory.h"
#include "engine/ee/cpu/ps2_float.h"
namespace save_callers {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) { Dispatch(address, context); }
'''
    dispatch = ["bool Run(uint32_t address, CpuContext& context) { switch (address) {"]
    for address in CALLERS:
        dispatch.append(f"case 0x{address:x}: FUN_{address:08x}(context, address); return true;")
    dispatch += ["default: return false; } }", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "save_callers.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "save_callers.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print("Save callers: four complete unchanged NEW bodies, explicit callee closure")


if __name__ == "__main__":
    main()
