"""Pin NTSC shadow/load ABIs and the complete generated static caller census."""
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
    0x1bcb58: "a248af051e46adc364b27ddf21c204238feb13d4024c1ee841da7b786f318eef",
    0x1be9f8: "04c1aa49441a9dd43d4229f08f4d13a38855688983ddcbb84ba28661fe6824c6",
    0x1c0b08: "d12c98d2a998268e78fa2c27daa60158bd9f259902671101805cb6f30716e2de",
    0x1c9048: "eb3c2ed8a798e8d7ce6c19f0e6b64bef4cac39f9086f9b8191deaacef3efd053",
    0x1c9ac0: "b256a3c045c79a33e3ff1d9ac02c48d0155b09b598640b75b548dfcbd782163c",
    0x27ae68: "0328b2b4b669a6347e46a187d43eeb1296b2f8530889e66a9da309ab623eeaf2",
    0x27aed8: "92b5c171fa27cc8d57c53e46d43adbffcfa58e0d37c0e832ca05a970ed4be6d5",
    0x1ca580: "f50cfcafc68c50209b3dd62104102e71c2177a43be169dcde264ed4106b91c49",
    0x1ca8b8: "399e5a40fc7b4c19bfa37cbf403c658b9ec4fcb18aa3f30e71ef1a57e176faf1",
}
CALLERS = {
    0x1bcb58: {0x1a5bd0, 0x1cc000},
    0x1c0b08: {0x1a48e8, 0x1c3070, 0x264948},
    0x1be9f8: {0x1ca8b8},
    0x1c9048: {0x1f1280},
    0x1c9ac0: {0x1f1280},
    0x1c8ed8: {0x268440},
    0x1ca580: {0x1c8ed8},
    0x27ae68: {0x1c9048},
    0x27aed8: {0x1c9ac0},
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
    calls = {at: [int(x, 16) for x in re.findall(
        r"recomp_call\(\d+u, (0x[0-9a-f]+)u, ctx\)", text)] for at, text in bodies.items()}
    for at, expected in CALLERS.items():
        actual = {caller for caller, targets in calls.items() if at in targets}
        if actual != expected:
            raise ValueError(f"shadow caller graph changed: {at:x}: {actual} != {expected}")
    for at, digest in PINS.items():
        if hashlib.sha256(bodies[at].encode()).hexdigest() != digest:
            raise ValueError(f"source ABI/body changed: {at:x}")
    words = {int(a, 16): int(b, 16) for a, b in re.findall(
        r"(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)", Path(require_private_fixture(DATA_MAP_REL)).read_text())}
    indirect = {at: [p for p, value in words.items() if value == at] for at in CALLERS}
    if any(indirect.values()):
        raise ValueError(f"new startup indirect ownership: {indirect}")
    # The type-0B shader vtable the NTSC factory stores (ReadMaterialShaders 1bb710:
    # shader+0x6c = 0x2fa8c8): GCC2 slot 0 reserved, slot 1 Destroy 1d9298, slot 5
    # Settings 1dc0e0, slot 6 Packet 1dc0e8, slot 8 NeedsEye 1d9330 (returns 1: the
    # default meshes carry eye-sized instances), slot 13 SetUp 1d9338.
    if (words.get(0x2fa8c8, 0) != 0 or words.get(0x2fa8cc, 0) != 0 or words.get(0x2fa8d4) != 0x1d9298 or
            words.get(0x2fa8f4) != 0x1dc0e0 or words.get(0x2fa8fc) != 0x1dc0e8 or
            words.get(0x2fa90c) != 0x1d9330 or words.get(0x2fa934) != 0x1d9338):
        raise ValueError("type-0B source material vtable changed")
    names = {}
    for row in (root / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        columns = row.split("\t")
        if len(columns) > 7 and columns[3] == "high":
            names.setdefault(columns[7], set()).add(int(columns[2], 16))
    for name, at in (
            ("ModelRead", 0x1bcb58), ("ModelDestroy", 0x1c0b08),
            ("SetDefaultMeshDMA", 0x1be9f8), ("StartShadows", 0x1c9048),
            ("ApplyShadows", 0x1c9ac0), ("UseHalfSizeScreen", 0x27ae68),
            ("RestoreViewScreenMatrix", 0x27aed8)):
        if at not in names.get(name, set()):
            raise ValueError(f"source PAL/NTSC mapping changed: {name}")
    print(json.dumps(dict(
        callers={f"{at:08x}": [f"{c:08x}" for c in sorted(cs)] for at, cs in CALLERS.items()},
        startup_indirect_entries=0, pinned_bodies=len(PINS),
        ownership="ModelRead: section reader + development-file loader; "
                  "default mesh: DrawShadowEntry only; begin/end: DrawShadows only",
        remaining="LoadDefault has both InitShadows(0) and InitShadows(1) source branches. "
                  "No unreachable claim for development files or dynamic code modifications.",
        scope="Static original executable corpus and startup data, not runtime retirement."),
        indent=2))


if __name__ == "__main__":
    main()
