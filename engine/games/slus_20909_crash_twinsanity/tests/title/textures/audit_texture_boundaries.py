#!/usr/bin/env python3
"""Static NEW corpus/NTSC data proof for the texture service's installed entries."""
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
    repo = Path(__file__).resolve().parents[5]
    title = repo / "games/slus_20909_crash_twinsanity"
    bodies = {}
    for path in sorted(recomp_dir(title.parents[1]).glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        starts = list(re.finditer(r"// Function: \w+ at (0x[0-9a-f]+)", text))
        for index, match in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            bodies[int(match[1], 16)] = text[match.end():end]
    calls = {
        address: [int(value, 16) for value in re.findall(
            r"recomp_call\(\d+u, (0x[0-9a-f]+)u, ctx\)", body)]
        for address, body in bodies.items()
    }
    expected = {
        0x1bc920: {0x19d248, 0x1a5b70, 0x1a6ac0, 0x1aa2e0},
        0x1c0988: {0x1a52c0, 0x1c1e18, 0x264948},
    }
    for target, callers in expected.items():
        actual = {address for address, targets in calls.items() if target in targets}
        assert actual == callers, (hex(target), actual, callers)
    assert calls[0x1a5b70] == [0x2b5cb0, 0x1bc920, 0x1a0cd8, 0x2b5d48]
    assert calls[0x1c0988] == [0x1816c8, 0x205488, 0x181590]
    words = {int(a, 16): int(b, 16) for a, b in re.findall(
        r"(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)", Path(require_private_fixture(DATA_MAP_REL)).read_text())}
    assert words[0x306b64] == 0x2b5dd8 and words[0x306bfc] == 0x2b5850
    # The startup map is sparse; absent words are zero-initialized by MainMemory.
    assert words.get(0x306b60, 0) == 0 and words.get(0x306bf8, 0) == 0
    assert "+ 0x6b50)" in bodies[0x2b5cb0]
    names = {}
    for row in (title / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        fields = row.split("\t")
        if len(fields) > 7 and fields[3] == "high" and fields[7]:
            names[fields[7]] = int(fields[2], 16)
    assert names["TextureRead"] == 0x1bc920 and names["TextureConstruct"] == 0x1c09f0
    cpp = (title / "hle/graphics/texture_bindings.cpp").read_text()
    table = re.search(r"kSettings\[\] = \{(.*?)\};", cpp, re.S)[1]
    entries = [int(word.strip(), 0) for word in table.split(",") if word.strip()]
    assert len(entries) == 33
    for index, entry in enumerate(entries):
        if index == 0x1d:
            assert entry == 0
            continue
        assert names[f"ShaderType{index:02X}Settings"] == entry
        slots = [at for at, value in words.items() if value == entry and 0x2f0000 < at < 0x300000]
        assert len(slots) == 1 and words.get(slots[0] - 4, 0) == 0
    # No other indirect entry to TextureRead/Destroy appears in startup data.
    indirect = {hex(target): [hex(at) for at, value in words.items() if value == target]
                for target in expected}
    assert all(not addresses for addresses in indirect.values()), indirect
    print(json.dumps({
        "read_callers": {
            "0x19d248": "particle page",
            "0x1a5b70": "graphics texture section",
            "0x1a6ac0": "Shape2D embedded texture",
            "0x1aa2e0": "font page",
        },
        "destroy_callers": {
            "0x1a52c0": "texture table ReleaseAll",
            "0x1c1e18": "texture table Release",
            "0x264948": "resource deletion step",
        },
        "settings_methods_verified": 32,
        "static_indirect_read_destroy_entries": indirect,
        "scope": "static corpus; does not assert runtime retirement or dynamically patched callers",
    }, indent=2))


if __name__ == "__main__":
    main()
