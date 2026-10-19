"""Extract complete unchanged NEW movie presentation bodies for MOV3."""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = (0x2B0E50, 0x2AF968, 0x2AEB30, 0x2B1448, 0x2B15F8, 0x2B1730)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    path = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_029.cpp")
    text = resolve(args.root, path).read_text()
    evidence, bodies = [], []
    for address in ENTRIES:
        start = text.index(f"void FUN_{address:08x}(")
        end = text.index("\n// Function:", start)
        body = text[start:end]
        bodies.append(body)
        digest = hashlib.sha256(body.encode()).hexdigest()
        evidence.append(dict(entry=f"0x{address:08x}", source=str(path),
                             line=text[:start].count("\n") + 1,
                             source_sha256=digest, compiled_sha256=digest, omitted=""))
    prefix = '''#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
#include "engine/ee/cpu/ps2_float.h"
namespace movie_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& context) { Dispatch(address, context); }
'''
    dispatch = ["bool RunGenerated(uint32_t address, CpuContext& context) {", "switch (address) {"]
    dispatch += [f"case 0x{a:x}: ObserveGenerated(address); FUN_{a:08x}(context, address); return true;"
                 for a in ENTRIES]
    dispatch += ["default: return false;", "}", "}", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "movie_graphics_generated.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "movie_graphics_generated.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"MOV3: {len(bodies)} complete unchanged NEW bodies")


if __name__ == "__main__":
    main()
