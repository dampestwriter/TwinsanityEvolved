"""Pin the complete source distortion producer and startup ownership in NTSC.

No playing/capturing is involved. These are ABI/caller checks; rendering and
retirement are established separately by the replacement and application gate.
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
    0x1b03f0: ("23fdc34393f023156d8b01804ebbdf7566752ef27ccb0dff72b6d91615366129", (), (0x1b5990,)),
    0x1b36b8: ("d48ad49f784c5c392fc05c1adf937a9622d3dbcda08b64fb7f032173c294c991", (), (0x1b3a70,)),
    0x1b3f70: ("aeefaf7972a59b240b9711ba779e7ef7afedfd529cecdf621d95334b3c3b0b74", (),
               (0x1b7088, 0x1b74a0, 0x1b7ca8)),
    0x1b47e0: ("90944a4100315cc759256f1716f4575e0f7c41321577654edf8222e886101af9", (),
               (0x1b6d48, 0x223490)),
    0x1b5cd8: ("2305880d2d8b1cbf6c67493cc2777a7730ed3b4d18358429b82793a5ffd52e1e", (), (0x1b74a0,)),
    0x1b5990: ("960801a9fd14998efd3f850de72d57ce6f99c009cd36107de53d51fba18a57ce", (), (0x1b74a0,)),
    0x1d5f90: ("885156385c50fd7db4571fafa140d87636d150e65ab7c8ade74d7f73feb4eadf", (0x2fa374,), ()),
    0x1d6318: ("f442632c542b443b9c711e32466d330bb7c09ad23614280616184b692ef89b4b", (0x2fa37c,), ()),
    0x1d9a68: ("c70e1316f5fa5b9a2571b5bade783a56d73dec57ff4d235891770da32e02d281", (0x2fa3b4,), (0x1b3a70,)),
    0x1dbf38: ("31ddb732e6c8c64fa49c61e2e4bb4512da67403606af5d3e14d2435db6be8dac", (), (0x1cc580,)),
    0x1dbfb0: ("f3579438da84464100758f0f1611759f42ec12e34aeb97c570f31016b21f4e93", (0x2fa3bc,), ()),
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
    for at, (digest, pointers, expected_callers) in PINS.items():
        if hashlib.sha256(bodies[at].encode()).hexdigest() != digest:
            raise ValueError(f"distortion ABI/body changed: {at:x}")
        if callers[at] != set(expected_callers) or sorted(a for a, v in words.items() if v == at) != list(pointers):
            raise ValueError(f"distortion caller/indirect owner changed: {at:x}")
        if any(words.get(pointer - 4, 0) for pointer in pointers):
            raise ValueError("distortion vtable this adjustment changed")
    names = {}
    for row in (root / "symbols/pal_ntsc_map.tsv").read_text().splitlines():
        fields = row.split("\t")
        if len(fields) > 7 and fields[3] == "high":
            names[fields[7]] = int(fields[2], 16)
    for name, address in (("DrawHexagonParticleBlock", 0x1b03f0), ("InitParticleBlocks", 0x1b36b8),
                          ("DrawParticleList", 0x1b5990), ("ShaderType18Settings", 0x1d5f90),
                          ("ShaderType18Packet", 0x1d6318), ("AllocParticleEmitterBlocks", 0x1b3f70),
                          ("FreeParticleEmitterBlocks", 0x1b47e0), ("ProcessParticleBlockEvents", 0x1b5cd8)):
        if names.get(name) != address:
            raise ValueError(f"distortion PAL/NTSC mapping changed: {name}")
    print(json.dumps(dict(complete_bodies=len(PINS), vtable="002fa348",
        source="DrawParticleList -> DrawHexagonParticleBlock; procedurally constructed type18 material.",
        geometry="Twelve records/block; 64 look steps, four ordered batches of three hexagons.",
        pending="Owned block-chain capture, complete copy/material selection, persistence, application gate.",
        scope="Static complete NEW executable and source ABI; no game execution."), indent=2))


if __name__ == "__main__":
    main()
