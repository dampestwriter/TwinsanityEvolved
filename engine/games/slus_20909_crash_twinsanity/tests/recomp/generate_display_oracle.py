"""Extract the complete NEW SetupPCRTC body with its presentation callers."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from recomp_paths import label, resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    relative = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_010.cpp")
    text = resolve(args.root, relative).read_text()
    start = text.index("void FUN_0019aff0(")
    body = text[start:text.index("\n// Function:", start)]
    if "recomp_call(" in body or "recomp_indirect_call(" in body:
        raise RuntimeError("SetupPCRTC acquired a callee: review source closure")
    callers = []
    for path in sorted(resolve(args.root, relative.parent).glob("recompiled_shard_*.cpp")):
        source = path.read_text()
        for match in re.finditer(r"recomp_call\(\d+u, 0x19aff0u,", source):
            begin = source.rfind("void FUN_", 0, match.start())
            caller = re.match(r"void FUN_([0-9a-f]{8})", source[begin:])[1]
            callers.append({"caller": "0x"+caller, "source": label(path, args.root),
                            "line": source[:match.start()].count("\n")+1})
    if [c["caller"] for c in callers] != ["0x001a00d0", "0x001a0150"]:
        raise RuntimeError("SetupPCRTC direct caller scope changed")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "display_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/display_oracle_memory.h"\nnamespace display_oracle {\n'+body+"\n}\n")
    digest = hashlib.sha256(body.encode()).hexdigest()
    (args.out / "display_generated.json").write_text(json.dumps({
        "entry": "0x0019aff0", "settings": "0x003d4ae0", "source": str(relative),
        "line": text[:start].count("\n")+1, "source_sha256": digest,
        "compiled_sha256": digest, "omitted": [], "substituted_callees": [],
        "direct_callers": callers,
        "remaining": ["indirect callers require runtime census",
                      "nonstandard circuit/position policy explicitly refuses selection",
                      "source present wait/timer boundaries remain separate phase work"],
    }, indent=2)+"\n")
    print("Display: complete unchanged NEW body; zero callees; two presentation callers")


if __name__ == "__main__":
    main()
