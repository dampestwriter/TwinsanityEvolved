"""Extract NEW generated producers for the isolated same-input Strip oracle.

This does not copy the readonly decomp implementation or open any external
recording. The manifest records every extracted body, its source location and
hash, and the sole removed host logging prelude.
"""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


BODIES = [
    (0, 0x101728), (0, 0x101748), (0, 0x101768), (0, 0x101788),
    (9, 0x18E4F0), (9, 0x18E570), (10, 0x19EF90), (10, 0x1A0640),
    (11, 0x1A6068), (11, 0x1A6190), (11, 0x1A8F78),
    (11, 0x1A9120), (11, 0x1A92A0), (11, 0x1A9448),
    (11, 0x1A9660), (11, 0x1AADB8), (11, 0x1AAE28),
    (12, 0x1C0278), (14, 0x1D3F38), (14, 0x1D88D8), (14, 0x1DBB38),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sources, evidence = [], []
    for shard, address in BODIES:
        path = Path("games/slus_20909_crash_twinsanity/recomp") / (
            f"recompiled_shard_{shard:03d}.cpp")
        text = resolve(args.root, path).read_text()
        name = f"FUN_{address:08x}"
        start = text.index(f"void {name}(")
        end = text.index("\n// Function:", start)
        original = body = text[start:end]
        omitted = ""
        if address == 0x18E4F0:
            # The logging prelude references the full host runtime; all emitted
            # entry-label arithmetic and generated return statements stay intact.
            body = f"void {name}(CpuContext& ctx, uint32_t addr) {{\n" + (
                body[body.index("Label_0000:"):])
            omitted = "host logging prelude before Label_0000"
        sources.append(body)
        evidence.append({
            "entry": f"0x{address:08x}", "source": str(path),
            "line": text[:start].count("\n") + 1,
            "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
            "compiled_sha256": hashlib.sha256(body.encode()).hexdigest(),
            "omitted": omitted,
        })
    prologue = """#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "engine/platform/host/platform.h"
PS2R_FENV_ACCESS_ON
namespace strip_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) {
    RunGenerated(address, context);
}
"""
    dispatch = ["void RunGenerated(uint32_t address, CpuContext& context) {",
                "ObserveGenerated(address);", "switch (address) {"]
    for _, address in BODIES:
        dispatch.append(
            f"case 0x{address:x}: FUN_{address:08x}(context, address); return;")
    dispatch += ["default: ExternalCall(address, context); return;", "}", "}", "}"]
    (args.out / "strip_generated.cpp").write_text(
        prologue + "\n".join(sources + dispatch) + "\n")
    (args.out / "strip_generated.json").write_text(
        json.dumps({"oracle": "NEW generated source", "bodies": evidence}, indent=2) + "\n")
    print(f"Strip NEW-body oracle: extracted {len(evidence)} functions")


if __name__ == "__main__":
    main()
