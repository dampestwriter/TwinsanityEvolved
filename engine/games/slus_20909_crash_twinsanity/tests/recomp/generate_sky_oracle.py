"""Extract the complete NEW generated FinishSky producer without substitutions."""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    relative = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_012.cpp")
    source = resolve(args.root, relative).read_text()
    start = source.index("void FUN_001b9f30(")
    end = source.index("\n// Function:", start)
    body = source[start:end]
    if "recomp_call(" in body or "recomp_indirect_call(" in body:
        raise RuntimeError("FinishSky acquired a callee: review oracle closure")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "sky_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/sky_oracle_memory.h"\nnamespace sky_oracle {\n' + body + "\n}\n")
    (args.out / "sky_generated.json").write_text(json.dumps({
        "oracle": "NEW generated source", "entry": "0x001b9f30", "source": str(relative),
        "line": source[:start].count("\n") + 1,
        "source_sha256": hashlib.sha256(body.encode()).hexdigest(),
        "compiled_sha256": hashlib.sha256(body.encode()).hexdigest(),
        "omitted": [], "substituted_callees": [],
    }, indent=2) + "\n")
    print("Sky NEW-body oracle: extracted complete FinishSky, no substitutions")


if __name__ == "__main__":
    main()
