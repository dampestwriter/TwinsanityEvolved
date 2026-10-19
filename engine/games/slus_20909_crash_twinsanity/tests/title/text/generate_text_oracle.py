"""Build-directory copies of current NEW's complete font producer chain."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import resolve  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

BODIES = [
    (6, 0x164f50), (9, 0x18e570),
    (10, 0x19b480), (10, 0x19d138), (10, 0x19ef90), (10, 0x1a0bf8),
    (11, 0x1a98b8), (11, 0x1a9c70), (11, 0x1aa638),
    (11, 0x1aa820), (11, 0x1aa900), (11, 0x1ac7a0), (11, 0x1ac7c0),
    (13, 0x1ce778), (13, 0x1ceb10), (14, 0x1da2d8),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source, provenance = [], []
    for shard, address in BODIES:
        path = Path(f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03}.cpp")
        text = resolve(args.root, path).read_text()
        start = text.index(f"void FUN_{address:08x}(")
        end = text.index("\n// Function:", start)
        body = text[start:end]
        source.append(body)
        provenance.append(dict(entry=hex(address), path=str(path),
                               line=text[:start].count("\n") + 1,
                               sha256=hashlib.sha256(body.encode()).hexdigest()))
    prelude = """#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "engine/platform/host/platform.h"
PS2R_FENV_ACCESS_ON
namespace strip_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& c) { RunGenerated(address, c); }
"""
    dispatch = ["void RunGenerated(uint32_t address, CpuContext& c) { ObserveGenerated(address); switch (address) {"]
    dispatch += [f"case 0x{a:x}: FUN_{a:08x}(c, address); return;" for _, a in BODIES]
    dispatch += ["default: ExternalCall(address, c); return;", "}}", "}"]
    (args.out / "text_generated.cpp").write_text(prelude + "\n".join(source + dispatch) + "\n")
    (args.out / "text_generated.json").write_text(json.dumps(dict(
        oracle="current NEW generated source, no omitted statements",
        external=["bounded StartMaterialWriter allocation", "settings/shared-frame allocator"],
        bodies=provenance), indent=2) + "\n")


if __name__ == "__main__":
    main()
