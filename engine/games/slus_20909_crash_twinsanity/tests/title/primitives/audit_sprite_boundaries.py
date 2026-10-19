#!/usr/bin/env python3
"""Verify the Sprite source boundary's exact static NTSC dispatch ownership."""
import json
import re
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import recomp_dir  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tests/lib"))
from private_fixtures import require_private_fixture  # noqa: E402

# The title's .data word dump is a private fixture (game bytes): SKIP 77 when absent.
DATA_MAP_REL = "games/slus_20909_crash_twinsanity/data/ram_data_map_v50.txt"


def main():
    root = Path(__file__).resolve().parents[5]
    title = root / "games/slus_20909_crash_twinsanity"
    entries = {
        0x1a8550: (0x1a8ce0, 0x2f6b7c, "Draw()"),
        0x1a87a0: (0x1a8f30, 0x2f6b84, "Draw(colour)"),
        0x1abeb0: (0x1ac640, 0x2f6b8c, "Draw(matrix)"),
        0x1a89e0: (0x1a9170, 0x2f6b94, "Draw(matrix,colour)"),
    }
    bodies = {}
    for path in sorted(recomp_dir(title.parents[1]).glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        starts = list(re.finditer(r"// Function: \w+ at (0x[0-9a-f]+)", text))
        for index, match in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            bodies[int(match[1], 16)] = text[match.end():end]
    calls = {address: set(int(value, 16) for value in re.findall(
        r"recomp_call\(\d+u, (0x[0-9a-f]+)u, ctx\)", body))
        for address, body in bodies.items()}
    words = {int(a, 16): int(b, 16) for a, b in re.findall(
        r"(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)",
        Path(require_private_fixture(DATA_MAP_REL)).read_text())}
    mapping = {}
    for line in (title / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        row = line.split("\t")
        if len(row) > 3 and row[3] == "high":
            mapping[int(row[0], 16)] = int(row[2], 16)
    report = {}
    for entry, (pal, slot, name) in entries.items():
        assert mapping[pal] == entry
        callers = [caller for caller, targets in calls.items() if entry in targets]
        assert not callers, (hex(entry), callers)
        locations = [at for at, target in words.items() if target == entry]
        assert locations == [slot] and words.get(slot - 4, 0) == 0
        assert 0x1a6068 in calls[entry] or entry == 0x1abeb0
        report[hex(entry)] = {"intent": name, "static_direct_callers": [],
                              "indirect_function_slot": hex(slot), "this_adjustment": 0}
    assert {0x18e4f0, 0x1a6068, 0x1a6190} <= calls[0x1a89e0]
    placed = bodies[0x1abeb0]
    assert 0x11bf10 in calls[0x1abeb0]
    assert "r[16].UL[0]) + 0x40)" in placed  # signed this-adjustment
    assert "r[16].SL[0]) + 0x40)" in placed  # advances to slot 8
    assert "r[16].UL[0]) + 0x4)" in placed   # slot's function pointer
    report["scope"] = "startup data/generated corpus; runtime patches and interior saved frames are not proven absent"
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
