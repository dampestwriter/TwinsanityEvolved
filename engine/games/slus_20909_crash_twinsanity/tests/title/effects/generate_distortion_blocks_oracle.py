"""Extract complete NEW game block operations and their software callees."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import label, resolve  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = {0x1b36b8, 0x1b3f70, 0x1b47e0, 0x1b5cd8, 0x1b9668, 0x1b97e0, 0x1b9848,
           0x2c6e88, 0x2c6bc8, 0x2c68f0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    found = {}
    for path in sorted(resolve(args.root, "games/slus_20909_crash_twinsanity/recomp").glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        for match in re.finditer(r"^void FUN_([0-9a-f]{8})\(", text, re.M):
            entry = int(match[1], 16)
            if entry not in ENTRIES:
                continue
            end = text.find("\n// Function:", match.start())
            body = text[match.start():end if end >= 0 else len(text)]
            calls = {int(a, 16) for a in re.findall(r"recomp_call\(\d+u, (0x[0-9a-f]+)u", body)}
            if calls - ENTRIES - {0x1815c0} or "recomp_indirect_call(" in body:
                raise ValueError(f"distortion block callee graph changed at {entry:x}: {calls}")
            found[entry] = (body, dict(entry=f"{entry:08x}", source=label(path, args.root),
                sha256=hashlib.sha256(body.encode()).hexdigest(), calls=[f"{a:08x}" for a in sorted(calls)]))
    if set(found) != ENTRIES:
        raise ValueError("missing complete distortion block source bodies")
    declarations = [f"void FUN_{a:08x}(CpuContext&, uint32_t);" for a in sorted(ENTRIES)]
    dispatch = ["void RunGenerated(uint32_t at, CpuContext& c) { ObserveGenerated(at); switch(at) {"]
    dispatch += [f"case 0x{a:x}: FUN_{a:08x}(c, at); return;" for a in sorted(ENTRIES)]
    dispatch += ["default: ExternalCall(at, c); return;", "}}", "}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "distortion_blocks_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"\n'
        '#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_blocks_oracle.h"\n'
        'namespace strip_oracle {\nDistortionBlocksOracle g_vu0;\n' +
        "\n".join(declarations + [
            "void recomp_call(uint32_t, uint32_t at, CpuContext& c) { RunGenerated(at, c); }"] +
            [found[a][0] for a in sorted(ENTRIES)] + dispatch) + "\n")
    (args.out / "distortion_blocks_generated.json").write_text(json.dumps(dict(
        bodies=[found[a][1] for a in sorted(ENTRIES)], external={"001815c0": "bounded byte allocator",
            "VU0:00f0": "unchanged current NEW LibVu0SinCos service, matching public signature"},
        scope="Complete NEW CPU initialization, resize, free, event-wheel and memory helpers. "
              "Original tag links are examined only by the test oracle, never by the owner."), indent=2) + "\n")


if __name__ == "__main__":
    main()
