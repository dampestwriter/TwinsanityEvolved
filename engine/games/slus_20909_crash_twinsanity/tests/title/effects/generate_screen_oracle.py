"""Extract complete current NEW screen-effect producers for same-input oracles."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import resolve  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = (0x1AE698, 0x1AEF48, 0x1AF788, 0x1AFCA8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    relative = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_011.cpp")
    text = resolve(args.root, relative).read_text()
    bodies, provenance = [], []
    for entry in ENTRIES:
        start = text.index(f"void FUN_{entry:08x}(")
        end = text.index("\n// Function:", start)
        body = text[start:end]
        bodies.append(body)
        provenance.append(dict(entry=hex(entry), source=str(relative),
                               line=text[:start].count("\n") + 1,
                               sha256=hashlib.sha256(body.encode()).hexdigest()))
    prelude = """#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
namespace strip_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& c) { RunGenerated(address, c); }
"""
    dispatch = ["void RunGenerated(uint32_t address, CpuContext& c) { ObserveGenerated(address); switch(address) {"]
    dispatch += [f"case 0x{entry:x}: FUN_{entry:08x}(c, address); return;" for entry in ENTRIES]
    dispatch += ["default: ExternalCall(address, c); return;", "}}", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "screen_generated.cpp").write_text(prelude + "\n".join(bodies + dispatch) + "\n")
    (args.out / "screen_generated.json").write_text(json.dumps(dict(
        oracle=__doc__, bodies=provenance,
        excludes=["palette blending", "wave program allocation", "wave grid execution",
                  "texture/target storage and aliasing"]), indent=2) + "\n")


if __name__ == "__main__":
    main()
