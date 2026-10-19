"""Extract current NEW functions for the same-input Sprite SELF oracle.

TR is a read-only specification. This copies only current NEW's generated
bodies into the build directory, recording hashes and logging-only omissions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import resolve  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

BODIES = [
    (1, 0x11bf10), (9, 0x18e4f0), (9, 0x18e570), (10, 0x19ef90),
    (11, 0x1a8550), (11, 0x1a87a0), (11, 0x1a89e0), (11, 0x1abeb0),
    (11, 0x1aac90), (11, 0x1aadb8), (11, 0x1aae28), (11, 0x1aae98),
    (13, 0x1cdd10), (13, 0x1ce778), (13, 0x1cf7f0), (14, 0x1d3f38),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sources, evidence = [], []
    for shard, address in BODIES:
        path = Path(f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03}.cpp")
        text = resolve(args.root, path).read_text()
        start = text.index(f"void FUN_{address:08x}(")
        end = text.index("\n// Function:", start)
        original = body = text[start:end]
        omitted = ""
        if address == 0x18e4f0:
            body = f"void FUN_{address:08x}(CpuContext& ctx, uint32_t addr) {{\n" + body[body.index("Label_0000:"):]
            omitted = "logging prelude before Label_0000"
        if address in (0x1a89e0, 0x1abeb0):
            a = body.index("    { extern std::ofstream g_logFile;")
            b = body.index("uint64_t _native_saved_r16", a)
            body = body[:a] + body[b:]
            omitted = "LOGO host logging block; all generated instructions retained"
        sources.append(body)
        evidence.append({
            "entry": hex(address), "path": str(path), "line": text[:start].count("\n") + 1,
            "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
            "compiled_sha256": hashlib.sha256(body.encode()).hexdigest(), "omitted": omitted,
        })
    prologue = """#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "engine/platform/host/platform.h"
PS2R_FENV_ACCESS_ON
namespace strip_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) { RunGenerated(address, context); }
"""
    dispatch = ["void RunGenerated(uint32_t address, CpuContext& context) {",
                "ObserveGenerated(address);", "switch (address) {"]
    dispatch += [f"case 0x{address:x}: FUN_{address:08x}(context, address); return;" for _, address in BODIES]
    dispatch += ["default: ExternalCall(address, context); return;", "}", "}", "}"]
    (args.out / "sprite_generated.cpp").write_text(prologue + "\n".join(sources + dispatch) + "\n")
    (args.out / "sprite_generated.json").write_text(json.dumps({
        "oracle": "current NEW generated source",
        "external": ["Begin2D/End2D output allocation/extent", "settings allocator"],
        "bodies": evidence,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
