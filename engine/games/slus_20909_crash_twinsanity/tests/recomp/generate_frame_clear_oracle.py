"""Extract the complete NEW SetUpFrame body and audit its direct callers."""
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
    start = text.index("void FUN_0019f1c8(")
    body = text[start:text.index("\n// Function:", start)]
    if "recomp_call(" in body or "recomp_indirect_call(" in body:
        raise RuntimeError("SetUpFrame acquired a callee: review source closure")
    callers = []
    for path in sorted(resolve(args.root, relative.parent).glob("recompiled_shard_*.cpp")):
        source = path.read_text()
        for match in re.finditer(r"recomp_call\(\d+u, 0x19f1c8u,", source):
            begin = source.rfind("void FUN_", 0, match.start())
            caller = re.match(r"void FUN_([0-9a-f]{8})", source[begin:])[1]
            callers.append({"caller": "0x"+caller, "source": label(path, args.root),
                            "line": source[:match.start()].count("\n")+1})
    if [c["caller"] for c in callers] != ["0x0019b280"]*3 + ["0x002aa3e8"]*2 + ["0x002ae658"]*2:
        raise RuntimeError("SetUpFrame direct caller scope changed")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "frame_clear_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/frame_oracle_memory.h"\nnamespace frame_oracle {\n'+body+"\n}\n")
    digest = hashlib.sha256(body.encode()).hexdigest()
    (args.out / "frame_clear_generated.json").write_text(json.dumps({
        "entry": "0x0019f1c8", "source": str(relative), "line": text[:start].count("\n")+1,
        "source_sha256": digest, "compiled_sha256": digest,
        "omitted": [], "substituted_callees": [], "direct_callers": callers,
        "remaining": ["indirect callers require runtime census",
                      "retained consumers inheriting source setup require closure before cutover"],
    }, indent=2)+"\n")
    print("Frame clear: complete unchanged NEW body; zero callees; seven direct call sites")


if __name__ == "__main__":
    main()
