"""Extract unchanged NEW WriteFrameHead for the source display-copy oracle."""
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
    start = text.index("void FUN_0019aaa8(")
    end = text.index("\n// Function:", start)
    body = text[start:end]
    if "recomp_call(" in body or "recomp_indirect_call(" in body:
        raise RuntimeError("WriteFrameHead acquired callees; review source closure")
    digest = hashlib.sha256(body.encode()).hexdigest()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "frame_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/frame_oracle_memory.h"\nnamespace frame_oracle {\n' + body + "\n}\n")
    (args.out / "frame_generated.json").write_text(json.dumps({
        "entry": "0x0019aaa8", "source": str(relative),
        "line": text[:start].count("\n") + 1,
        "source_sha256": digest, "compiled_sha256": digest,
        "omitted": [], "substituted_callees": [],
    }, indent=2) + "\n")
    audited = {"0019aaa8", "0019ade0", "00181968", "001819e8", "001a00d0", "001a0150"}
    bodies, callers = [], []
    for path in sorted(resolve(args.root, relative.parent).glob("recompiled_shard_*.cpp")):
        source = path.read_text()
        for match in re.finditer(r"void FUN_([0-9a-f]{8})\(", source):
            stop = source.find("\n// Function:", match.start())
            function = source[match.start():stop if stop >= 0 else len(source)]
            if match[1] in audited:
                bodies.append({"entry": "0x"+match[1], "source": label(path, args.root),
                               "line": source[:match.start()].count("\n")+1,
                               "sha256": hashlib.sha256(function.encode()).hexdigest()})
            for target in re.findall(r"recomp_call\(\d+u, 0x(19aaa8|181968|1819e8)u,", function):
                callers.append({"caller": "0x"+match[1], "target": "0x"+target.zfill(8)})
    expected = [{"caller": "0x0019ade0", "target": "0x0019aaa8"},
                {"caller": "0x001a00d0", "target": "0x001819e8"},
                {"caller": "0x001a0150", "target": "0x00181968"}]
    if callers != expected or len(bodies) != len(audited):
        raise RuntimeError("Frame direct caller scope changed; audit before acceptance")
    (args.out / "frame_callers.json").write_text(json.dumps({
        "bodies": bodies, "direct_callers": callers, "source_object": "0x003d4400",
        "scope": "complete head; ordinary/interrupt send only for the known source object",
        "remaining": ["foreign/indirect send clients", "Present pacing/display SDK calls",
                      "FrameBuckets Link and unselected source producers"],
    }, indent=2)+"\n")
    print("Frame: one complete unchanged NEW body, no substituted callees")


if __name__ == "__main__":
    main()
