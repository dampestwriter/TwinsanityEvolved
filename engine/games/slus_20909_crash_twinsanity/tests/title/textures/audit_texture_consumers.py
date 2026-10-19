#!/usr/bin/env python3
"""Enumerate every retail texture ID's complete static consumer families.

The material certificate answers "may MeshPass own this material"; this audit
answers the texture-side question the mesh and texture owners need: "which
families still read this texture through the original TextureRead/VRAM path".
A texture's original upload/lookup can retire only when EVERY family below has
its own composed native owner. Read-only schema authority: TR renderer/
resources.cpp, textures.cpp, models.cpp; game/{shadows,oleg,shapes}.cpp.
No game execution, packet interpretation or runtime seen-set is involved.
"""
import argparse
import collections
import hashlib
import json
import mmap
from pathlib import Path
import struct

from audit_material_consumers import inventory
from audit_texture_assets import archive_index, u32
from audit_texture_owners import raw_hits
import material_records as records

# Bit order is the TextureConsumers::Family contract; append only.
FAMILIES = [
    "certified-placed", "placed", "rigid", "skin", "blend-skin", "sky", "shadow-default",
    "sprite", "font", "particle", "borrowed-hud", "cloth",
    "opaque-reference", "unreferenced", "missing-record", "orphan-material",
]
EXCLUSION_FAMILY = {
    "skin": "skin", "blend-skin": "blend-skin", "sky": "sky", "shadow-default": "shadow-default",
    "sprite": "sprite", "font": "font", "particle": "particle",
    "potential-borrowed-hud": "borrowed-hud", "opaque-reference": "opaque-reference",
}
CLOTH = {0x17, 0x1a}


def bit(name):
    return 1 << FAMILIES.index(name)


def classify(materials, draws, excluded, pair_textures, certified, texture_ids, foreign):
    """Pure per-texture family closure over the complete inventory.

    materials: id -> {sha: {record}}; draws: rigid/placed records; excluded:
    id -> exclusion reasons; pair_textures: (texture, family) from PSM/PSF/PTC
    headers; certified: certified material IDs; texture_ids: every texture
    record ID in the archive; foreign: texture IDs hit in opaque payloads.
    """
    material_families = collections.defaultdict(int)
    for draw in draws:
        family = "rigid" if draw["kind"] == 3 else "placed"
        for mid in draw["materials"]:
            # A certified material's placed consumers are the certified family;
            # the plain placed/rigid bits mean an UNCERTIFIED mesh consumer.
            material_families[mid] |= bit("certified-placed" if mid in certified else family)
    for mid in certified:
        if mid not in material_families:
            raise ValueError(f"certified material {mid:08x} has no placed consumer")
    for mid, reasons in excluded.items():
        for reason in reasons:
            if reason in EXCLUSION_FAMILY:
                material_families[mid] |= bit(EXCLUSION_FAMILY[reason])
    textures = collections.defaultdict(lambda: dict(families=0, shaders=0, materials=set()))
    for mid, variants in materials.items():
        for variant in variants.values():
            for shader in variant["record"]["shaders"]:
                tid = shader["texture"]
                if not tid:
                    continue
                entry = textures[tid]
                entry["materials"].add(mid)
                entry["shaders"] |= 1 << shader["type"]
                # A material with no statically found consumer still names
                # this texture; it stays retained until a source disposition.
                entry["families"] |= material_families.get(mid, 0) or bit("orphan-material")
                if shader["type"] in CLOTH:
                    entry["families"] |= bit("cloth")
    for tid, family in pair_textures:
        textures[tid]["families"] |= bit(family)
    for tid in foreign:
        textures[tid]["families"] |= bit("opaque-reference")
    for tid in texture_ids:
        if tid not in textures:
            textures[tid]["families"] |= bit("unreferenced")
    for tid, entry in textures.items():
        if tid not in texture_ids:
            entry["families"] |= bit("missing-record")
        if not entry["families"]:
            raise ValueError(f"texture {tid:08x} has a material consumer without a family")
    return textures


def pair_headers(data, index):
    """Texture/material pairs declared directly by PSM/PSF/PTC headers.

    Tail extents of these files are verified by audit_material_consumers.
    """
    pairs = []
    for name, start, size in archive_index(index):
        ext = Path(name).suffix.lower()
        if ext == ".ptc":
            pairs.append((u32(data, start), "particle"))
        elif ext in (".psm", ".psf"):
            at = start
            if ext == ".psm":
                count = 40 if name.lower() == "startup\\icons.psm" else (
                    1 if name.lower().startswith("language\\titles\\") else 8)
            else:
                count = u32(data, at)
                at += 4
            for _ in range(count):
                tid, _mid = struct.unpack_from("<II", data, at)
                pairs.append((tid, "sprite" if ext == ".psm" else "font"))
                at += 8
                at += 4 + u32(data, at)
                _parsed, at = records.material(data, at, start + size - at, complete=False)
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--textures", required=True, type=Path, help="complete textures.json")
    parser.add_argument("--materials", required=True, type=Path, help="complete materials.json")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = (args.archive / "CRASH.BH").read_bytes()
    header_sha = hashlib.sha256(index).hexdigest()
    ti = json.loads(args.textures.read_text())
    mi = json.loads(args.materials.read_text())
    if ti["archive_header_sha256"] != header_sha or mi["archive_header_sha256"] != header_sha:
        raise ValueError("inventory archive mismatch")
    texture_ids = {int(v["id"], 16) for v in ti["appearances"]}
    appearances = collections.Counter(int(v["id"], 16) for v in ti["appearances"])
    certified = {int(row["material"], 16) for row in mi["certified"]}
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        data_sha = hashlib.sha256(data).hexdigest()
        if mi["archive_data_sha256"] != data_sha:
            raise ValueError("material inventory data archive mismatch")
        materials, _models, draws, excluded, _pairs, spans, _counts = inventory(data, index)
        pairs = pair_headers(data, index)
        all_material_ids = set(materials)
        foreign = set()
        for _name, _kind, at, size in spans:
            for word, _offset in raw_hits(data, at, size, texture_ids | all_material_ids):
                if word in texture_ids:
                    foreign.add(word)
    textures = classify(materials, draws, excluded, pairs, certified, texture_ids, foreign)
    rows = []
    for tid in sorted(textures):
        entry = textures[tid]
        rows.append(dict(texture=f"{tid:08x}", families=entry["families"],
                         shaders=entry["shaders"], materials=len(entry["materials"]),
                         appearances=appearances.get(tid, 0)))
    family_counts = {name: sum(1 for r in rows if r["families"] & bit(name)) for name in FAMILIES}
    shader_counts = {f"{t:02x}": sum(1 for r in rows if r["shaders"] >> t & 1) for t in range(64)
                     if any(r["shaders"] >> t & 1 for r in rows)}
    exclusive_certified = sum(1 for r in rows if r["families"] == bit("certified-placed"))
    certified_textures = {int(row["texture"], 16) for row in mi["certified"]}
    for row in mi["certified"]:
        families = textures[int(row["texture"], 16)]["families"]
        if row["texture_exclusive"] != (families == bit("certified-placed")):
            raise ValueError(f"certificate exclusivity disagrees for texture {row['texture']}")
    report = dict(archive_header_sha256=header_sha, archive_data_sha256=data_sha,
                  families=FAMILIES, texture_ids=len(rows), appearances=sum(appearances.values()),
                  family_counts=family_counts, shader_type_counts=shader_counts,
                  exclusive_certified_textures=exclusive_certified,
                  certified_textures=len(certified_textures),
                  single_family_textures=sum(1 for r in rows if bin(r["families"]).count("1") == 1),
                  rows=rows,
                  scope="Complete retail CRASH.BH/CRASH.BD static consumer families per texture ID. "
                        "A family bit means the original TextureRead/VRAM path still serves that "
                        "consumer until its native owner is composed and selected. No readiness, "
                        "generation or retirement is implied by this inventory.")
    (args.out / "texture_consumers.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(rows)} texture IDs; {exclusive_certified} exclusive certified; "
          f"{report['single_family_textures']} single-family")
    print(json.dumps(family_counts, sort_keys=True))


if __name__ == "__main__":
    main()
