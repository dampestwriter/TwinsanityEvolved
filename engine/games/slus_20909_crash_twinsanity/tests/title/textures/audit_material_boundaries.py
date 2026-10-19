#!/usr/bin/env python3
"""Pin the NTSC code/data edges supporting the static material certificate."""
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity/tests/recomp"))
from recomp_paths import recomp_dir  # noqa: E402  the generated recomp TUs (PS2R_RECOMP_DIR)

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tests/lib"))
from private_fixtures import require_private_fixture  # noqa: E402

# The title's .data word dump is a private fixture (game bytes): SKIP 77 when absent.
DATA_MAP_REL = "games/slus_20909_crash_twinsanity/data/ram_data_map_v50.txt"
import re


def main():
    title = Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity"
    bodies = {}
    for path in sorted(recomp_dir(title.parents[1]).glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        matches = list(re.finditer(r"// Function: \w+ at (0x[0-9a-f]+)", text))
        for index, match in enumerate(matches):
            end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
            bodies[int(match[1], 16)] = text[match.end():end]
    calls = {address: set(int(value, 16) for value in re.findall(
        r"recomp_call\(\d+u, (0x[0-9a-f]+)u, ctx\)", body))
        for address, body in bodies.items()}
    words = {int(a, 16): int(b, 16) for a, b in re.findall(
        r"(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)", Path(require_private_fixture(DATA_MAP_REL)).read_text())}
    maps = {}
    for row in (title / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        fields = row.split("\t")
        if len(fields) > 3 and fields[3] == "high":
            maps[int(fields[0], 16)] = int(fields[2], 16)
    for pal, ntsc in {
        0x1ab7f0: 0x1ab060, 0x1ab938: 0x1ab1a8, 0x1ac5b0: 0x1abe20,
        0x1c0e00: 0x1c0670, 0x171008: 0x1711a8, 0x1cad10: 0x1ca580,
    }.items():
        assert maps[pal] == ntsc
    expected = {
        0x1ab060: {0x15bdb0},  # ScreenModelMaterial <- LaySkidMark only
        0x1be9f8: {0x1ca8b8},  # default <- shadow
        0x1ba3a8: {0x1c0490, 0x1a6298},  # world material flush AND screen flush
        0x1711a8: {0x121138, 0x176d40, 0x176e30},  # script/boss/whackaworm HUD
        0x1ab1a8: set(), 0x1abe20: set(),  # virtual material-ID setters
        0x1ca580: {0x1c8ed8},  # shadow startup
    }
    for target, callers in expected.items():
        assert {a for a, values in calls.items() if target in values} == callers, hex(target)
    data_edges = {
        0x1ab060: [], 0x1be9f8: [], 0x1ba3a8: [],
        0x1ab1a8: [0x2f694c, 0x2f69a4, 0x2f6bc4],
        0x1abe20: [0x2f6b6c],
    }
    for target, edges in data_edges.items():
        assert sorted(a for a, value in words.items() if value == target) == edges
    # LaySkidMark chooses standalone adding/subtracting skid materials.
    for offset in ("-0x7f54", "-0x7f50"):
        assert f"r[28].UL[0]) + {offset})" in bodies[0x15bdb0]
    # Shape2D::Read dispatches SetMaterialById slot 3 after loading its file pair.
    assert "+ 0x1c)" in bodies[0x1a6ac0] and "0x1a0cd8" in bodies[0x1a6ac0]
    # MaterialResourceRead creates a separate Material, call mode 1, then
    # publishes Material* into resource+8. MeshPass takes that pointee.
    body = bodies[0x1c0670]
    for text in ("+ 0x60,", "+ 0x64,", "+ 0x65,", "+ 0x66,", "0x1bb710u",
                 "r[18].UL[0] + 0x8, ctx.cpuRegs.GPR.r[16].UL[0]"):
        assert text in body
    checked = set(expected) | {0x15bdb0, 0x1a6ac0, 0x1c0670}
    print(json.dumps(dict(
        direct_callers={f"{target:08x}": [f"{a:08x}" for a in sorted(callers)]
                        for target, callers in expected.items()},
        body_sha256={f"{a:08x}": hashlib.sha256(bodies[a].encode()).hexdigest() for a in sorted(checked)},
        limits="Generated direct calls and startup data words, paired with TR source consumer analysis. "
               "Computed pointer injection, foreign assets and changed source are not authorized.",
    ), indent=2))


if __name__ == "__main__":
    main()
