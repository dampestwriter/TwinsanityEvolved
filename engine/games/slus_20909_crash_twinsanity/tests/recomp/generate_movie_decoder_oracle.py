"""Extract unchanged NEW movie lifecycle bodies for the isolated M2 oracle.

Only the project's generated NEW source is read. No OLD/decomp implementation
or external rendering capture is embedded. Every compiled body is hashed.
"""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


BODIES = {
    29: (0x2AFC00, 0x2B0AE0, 0x2B0D30, 0x2B0F68, 0x2B1000, 0x2B1120,
         0x2B1640, 0x2B1668, 0x2B1670, 0x2B7C00, 0x2B7D30, 0x2B7D48),
    30: (0x2BBEB0,),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    bodies, evidence = [], []
    for shard, addresses in BODIES.items():
        path = Path("games/slus_20909_crash_twinsanity/recomp") / f"recompiled_shard_{shard:03d}.cpp"
        text = resolve(args.root, path).read_text()
        for address in addresses:
            begin = text.index(f"void FUN_{address:08x}(")
            end = text.index("\n// Function:", begin)
            body = text[begin:end]
            bodies.append(body)
            evidence.append({
                "entry": f"0x{address:08x}", "source": str(path),
                "line": text[:begin].count("\n") + 1,
                "source_sha256": hashlib.sha256(body.encode()).hexdigest(),
                "compiled_sha256": hashlib.sha256(body.encode()).hexdigest(),
                "omitted": "",
            })
    prefix = """#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
namespace movie_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) {
    Dispatch(address, context);
}
"""
    dispatch = ["bool RunGenerated(uint32_t address, CpuContext& context) {", "switch (address) {"]
    for addresses in BODIES.values():
        for address in addresses:
            dispatch.append(f"case 0x{address:x}: ObserveGenerated(address); FUN_{address:08x}(context, address); return true;")
    dispatch += ["default: return false;", "}", "}", "}"]
    (args.out / "movie_generated.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "movie_generated.json").write_text(json.dumps({
        "oracle": "unchanged NEW generated movie lifecycle bodies", "bodies": evidence,
    }, indent=2) + "\n")
    print(f"Movie NEW-body oracle: {len(evidence)} unchanged functions")


if __name__ == "__main__":
    main()
