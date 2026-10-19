"""Pin the complete NTSC cloth read/update/settings/producer ABI.

Together with the complete archive audit, these are source ownership evidence,
not a runtime retirement claim. Shader frame updates are source software; the
selected geometry/material path must not execute settings/packet producers.
"""
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

PINS = {
    0x1d0d20: ("4a6708066e757da242553147215702597a25da811ec7ce607c5a7015012221f5", 0x2fa254),
    0x1d11b8: ("b9540a7ccce7f431028bf1ee9833c02bda5c2c4fd227aef3c5ed78a2d2b30f52", 0x2fa25c),
    0x1d1560: ("13fb0e41f9b42945d09e3255b9fe63d6d8839e69ff257517ed1db02409bb8dfb", 0x2fa244),
    0x1d18c8: ("082e6d9f3abf5fc5d4c2224bb4035b0d1a86c4579fef7ceb5fa678402e2e3e7c", 0x2fa404),
    0x1d1d48: ("c19a528047c5c72eb9051022b0af33e3a0ca9df040183b38269ba905d5844250", 0x2fa40c),
    0x1d20b8: ("ba8fc355f84f200e633a5387ac685a2ff2d97742b955edc8dae80346bf104c47", 0x2fa3f4),
    0x1db3e8: ("b122f3771a9b00ee5d27043f47ae40b683a4b4d4ba8968894cf48a1257fa7bfe", 0x2fa29c),
    0x1db668: ("6e03c2c4bed9928a16f5379fcd4fb8c1914c33bb7f73c136e7f026dba395a47b", 0x2fa44c),
}


def main():
    root = Path(__file__).resolve().parents[5] / "games/slus_20909_crash_twinsanity"
    bodies = {}
    for path in sorted(recomp_dir(root.parents[1]).glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        starts = list(re.finditer(r"// Function: \w+ at (0x[0-9a-f]+)", text))
        for i, match in enumerate(starts):
            end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
            bodies[int(match[1], 16)] = text[match.end():end]
    words = {int(a, 16): int(b, 16) for a, b in re.findall(
        r"(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)", Path(require_private_fixture(DATA_MAP_REL)).read_text())}
    callers = {at: set() for at in PINS}
    for caller, body in bodies.items():
        for address in re.findall(r"recomp_call\(\d+u, (0x[0-9a-f]+)u, ctx\)", body):
            if int(address, 16) in callers:
                callers[int(address, 16)].add(caller)
    for at, (sha, pointer) in PINS.items():
        if hashlib.sha256(bodies[at].encode()).hexdigest() != sha:
            raise ValueError(f"cloth ABI/body changed: {at:x}")
        if callers[at] or sorted(a for a, value in words.items() if value == at) != [pointer]:
            raise ValueError(f"cloth indirect ownership changed: {at:x}")
        if words.get(pointer - 4, 0):
            raise ValueError("cloth vtable this adjustment")
    names = {}
    for row in (root / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        fields = row.split("\t")
        if len(fields) > 7 and fields[3] == "high":
            names[fields[7]] = int(fields[2], 16)
    for family, entries in (
            ("17", (0x1d18c8, 0x1d1d48, 0x1d20b8, 0x1db668)),
            ("1A", (0x1d0d20, 0x1d11b8, 0x1d1560, 0x1db3e8))):
        for suffix, at in zip(("Settings", "Packet", "Update", "Read"), entries):
            if names.get(f"ShaderType{family}{suffix}") != at:
                raise ValueError("cloth source PAL/NTSC mapping changed")
    print(json.dumps(dict(complete_bodies=len(PINS), direct_callers=0,
        vtables={"17": "002fa3d8", "1A": "002fa228"},
        source_geometry="Cloth deformation precedes common strip gate, six-plane clipping and projection.",
        source_consumers="Archive audit: all 50 cloth versions are placed meshes; "
                         "three material IDs also have ordinary shader versions.",
        remaining="Frame/material producer selection, owned resource composition and application gate.",
        scope="Static generated code and startup indirect entries; no game execution."), indent=2))


if __name__ == "__main__":
    main()
