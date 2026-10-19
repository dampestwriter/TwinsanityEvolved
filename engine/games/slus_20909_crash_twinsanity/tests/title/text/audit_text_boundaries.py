#!/usr/bin/env python3
"""Pin the whole text producer and its static NTSC dispatch closure."""
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
    title = Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity"
    entries = {
        0x19d138: (0x19d8c8, None, "DrawFontTexts", [0x19b120]),
        0x1ac7a0: (0x1acf30, 0x2f67f4, "Font::Begin", []),
        0x1ac7c0: (0x1acf50, 0x2f67fc, "Font::Draw", []),
        0x1aa900: (0x1ab090, 0x2f6804, "Font::End", []),
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
    for entry, (pal, slot, name, expected_callers) in entries.items():
        assert mapping[pal] == entry
        callers = [caller for caller, targets in calls.items() if entry in targets]
        assert callers == expected_callers, (hex(entry), callers)
        locations = [at for at, target in words.items() if target == entry]
        assert locations == ([slot] if slot else []), (hex(entry), locations)
        if slot:
            assert words.get(slot - 4, 0) == 0
        report[hex(entry)] = dict(intent=name, direct_callers=list(map(hex, callers)),
                                  indirect_slot=hex(slot) if slot else None)
    assert calls[0x1ac7a0] == {0x1aa820}
    assert calls[0x1ac7c0] == {0x164f50, 0x1a9c70}
    assert calls[0x1aa900] == {0x1aa820}
    assert calls[0x1a9c70] == {0x164f50, 0x1a98b8, 0x1aa638}
    assert calls[0x1aa820] == {0x1c0190}
    # The sole enclosing producer dispatches the actual font methods at the
    # checked slots; its iterator's Current method is the other indirect call.
    body = bodies[0x19d138]
    for access in ("r[2].UL[0]) + 0x14)", "r[7].UL[0]) + 0x1c)", "r[2].UL[0]) + 0x24)"):
        assert access in body
    assert words[0x2f5f2c] == 0x1a0bf8 and words.get(0x2f5f28, 0) == 0
    report["scope"] = ("current generated corpus/startup data; dynamic patches and saved interior "
                       "frames remain explicit integration dispositions")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
