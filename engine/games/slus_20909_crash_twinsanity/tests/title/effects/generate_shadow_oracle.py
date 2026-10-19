"""Extract the current NEW shadow pass producers, never a captured draw stream."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import resolve  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = {0x1C9048: 13, 0x1C9AC0: 13, 0x27AE68: 25, 0x27AED8: 25,
           0x1A1DE8: 10, 0x18E570: 9, 0x18E5A0: 9}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source, provenance = [], []
    for address, shard in ENTRIES.items():
        relative = Path(f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03}.cpp")
        text = resolve(args.root, relative).read_text()
        start = text.index(f"void FUN_{address:08x}(")
        end = text.index("\n// Function:", start)
        body = text[start:end]
        source.append(body)
        provenance.append(dict(entry=hex(address), path=str(relative),
                               line=text[:start].count("\n") + 1,
                               sha256=hashlib.sha256(body.encode()).hexdigest()))
    prelude = """#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
namespace strip_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& c) { RunGenerated(address, c); }
"""
    dispatch = ["void RunGenerated(uint32_t address, CpuContext& c) { ObserveGenerated(address); switch (address) {"]
    dispatch += [f"case 0x{a:x}: FUN_{a:08x}(c, address); return;" for a in ENTRIES]
    dispatch += ["default: ExternalCall(address, c); return;", "}}", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "shadow_generated.cpp").write_text(prelude + "\n".join(source + dispatch) + "\n")
    (args.out / "shadow_generated.json").write_text(json.dumps(dict(
        oracle="current NEW complete generated pass bodies + NEW vertex assembly",
        excludes=["shadow geometry", "texture-slot allocation policy"],
        bodies=provenance), indent=2) + "\n")


if __name__ == "__main__":
    main()
