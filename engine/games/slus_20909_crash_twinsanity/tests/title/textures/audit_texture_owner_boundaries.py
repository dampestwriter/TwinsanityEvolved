#!/usr/bin/env python3
"""Check the concrete NTSC allocation/consumer boundaries used by UI selection."""
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
    for pal, ntsc in {0x1784a0: 0x178388, 0x1aaa70: 0x1aa2e0,
                      0x171008: 0x1711a8, 0x1daaa8: 0x1da318,
                      0x1a2750: 0x1a1fc0, 0x1a1468: 0x1a0cd8}.items():
        assert maps[pal] == ntsc
    assert [a for a, targets in calls.items() if 0x1aa2e0 in targets] == [0x178388]
    assert {0x1c09f0, 0x1bc920, 0x1a0cd8} <= calls[0x1aa2e0]
    assert {0x1c09f0, 0x1bc920, 0x1a0cd8} <= calls[0x1a6ac0]
    assert "r[16].SL[0]) + 0x68)" in bodies[0x178388]
    assert "r[28].UL[0]) + -0x7e1c)" in bodies[0x178388]
    address = words[0x3096d4]  # actual gp 0x3114f0 - 0x7e1c
    name = bytearray()
    for index in range(128):
        byte = (words.get((address + index) & ~3, 0) >> (((address + index) & 3) * 8)) & 255
        if not byte:
            break
        name.append(byte)
    assert bytes(name) == b"StartUp\\Fonts\\Crash"
    # Shape2D::Read is shared by four startup vtables; only one is Sprite.
    assert [a for a, value in words.items() if value == 0x1a6ac0] == [
        0x2f697c, 0x2f69d4, 0x2f6b9c, 0x2f6bf4]
    assert words[0x2f6b9c] == 0x1a6ac0 and words.get(0x2f6b98, 0) == 0
    # Runtime texture reassignments derive from particle-page materials. They
    # are not an authorization to convert their retained world texture.
    assert [a for a, targets in calls.items() if 0x1da318 in targets] == [0x16d368, 0x19d248]
    for entry in (0x1da318, 0x1ccf80):
        assert "0x3b0000" in bodies[entry] and "+ 0x240)" in bodies[entry]
        assert "+ 0x14)" in bodies[entry] and "+ 0x24)" in bodies[entry]
    assert [a for a, value in words.items() if value == 0x1c1c78] == [
        0x2f8dec, 0x2f8e3c, 0x2f93b4]
    assert words.get(0x2f8de8, 0) & 0xffff == 0 and words[0x2f8dec] == 0x1c1c78
    # Source table layout: u16 count +4, entries pointer +0; each row is
    # {object, id} eight bytes. This checks actual NTSC field accesses too.
    assert "read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) + 0x4)" in bodies[0x1a1fc0]
    assert "read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) + 0x0)" in bodies[0x1a1fc0]
    assert "<< 3)" in bodies[0x1a1fc0]
    assert "write<uint16_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x6" in bodies[0x1a0cd8]
    print(json.dumps(dict(
        primary_font=dict(load="0x1aa2e0", owner="GameController+0x68",
                          caller="0x178388", file=name.decode()),
        sprite=dict(vtable="0x2f6b50", read_slot="0x2f6b9c", read="0x1a6ac0"),
        borrowed_world_hud=dict(set_icon="0x1711a8", slots=[3, 4, 5]),
        texture_table="0x3b0240",
        dynamic_texture_set_callers=["0x16d368", "0x19d248"],
        scope="static NTSC code/data; catalogue and live ownership must also validate"), indent=2))


if __name__ == "__main__":
    main()
