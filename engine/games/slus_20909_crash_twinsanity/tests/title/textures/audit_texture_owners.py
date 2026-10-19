#!/usr/bin/env python3
"""Prove a conservative UI texture selection set from source-declared retail assets.

World material records are read by the source Shader reader's exact field
layout. Other chunk/resource bytes are scanned at every byte alignment: hits
are conservative exclusions, not invented semantic material references.
The initial startup catalogue is checked separately from all picture IDs.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import mmap
import struct

from audit_texture_assets import archive_index, section, u32


def material(data, start, size):
    limit = start + size
    if size < 20:
        raise ValueError("short material")
    programs, bucket, name_size = struct.unpack_from("<QII", data, start)
    at = start + 16 + name_size
    if at + 4 > limit:
        raise ValueError("material name extent")
    count = u32(data, at)
    at += 4
    if count > 4:
        raise ValueError("material shader count")
    shaders = []
    # ReadShader wrappers: screen copy, cloth, wave fields before the base.
    prefixes = {0x10: 4, 0x11: 4, 0x17: 12, 0x18: 4, 0x1a: 20, 0x1c: 8}
    for _ in range(count):
        if at + 4 > limit:
            raise ValueError("shader kind extent")
        kind = u32(data, at)
        if kind > 0x20:
            raise ValueError("unknown shader kind")
        at += 4 + prefixes.get(kind, 0)
        if at + 90 > limit or u32(data, at + 86) != kind:
            raise ValueError("base shader extent/type")
        if data[at + 29]:
            raise ValueError("animated shader requires a separately proven grammar")
        shaders.append(dict(type=kind, texture=u32(data, at + 82)))
        at += 90
    if at != limit:
        raise ValueError("unclassified material tail")
    return programs, bucket, shaders


def raw_hits(data, at, size, candidates):
    blob = data[at:at + size]
    for alignment in range(4):
        end = len(blob) - (len(blob) - alignment) % 4
        if end >= alignment:
            for n, (word,) in enumerate(struct.iter_unpack("<I", blob[alignment:end])):
                if word in candidates:
                    yield word, at + alignment + n * 4


def catalogue(path, entries, header_sha):
    rows = sorted(entries.items())
    lines = [
        "#pragma once", "", "#include <array>", "#include <cstdint>", "",
        "namespace title {", "",
        "// Retail startup asset identities. Selection still requires the matching",
        "// complete consumer service, owned generations and checkpoint preflight.",
        "// Generated/checked by tests/title/textures/audit_texture_owners.py.",
        "class TextureOwners {", "public:",
        "    enum class Family { CrashFont, ArialFont, Hud };",
        "    struct Entry { uint32_t texture, material; Family family; };",
        f'    static constexpr const char* kArchiveHeaderSha256 = "{header_sha}";',
        f"    static constexpr std::array<Entry, {len(rows)}> kStartup{{{{",
    ]
    for texture, record in rows:
        lines.append(f"        {{0x{texture:08x}, 0x{record['material']:08x}, Family::{record['family']}}},")
    lines += [
        "    }};", "    static const Entry* Find(uint32_t texture);",
        "};", "", "}  // namespace title", "",
    ]
    text = "\n".join(lines)
    if path.exists() and path.read_text() != text:
        raise ValueError("startup owner catalogue differs; review before regeneration")
    if not path.exists():
        path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--fonts", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--catalogue", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = (args.archive / "CRASH.BH").read_bytes()
    header_sha = hashlib.sha256(index).hexdigest()
    inventory = json.loads(args.inventory.read_text())
    fonts = json.loads(args.fonts.read_text())
    if any(value["archive_header_sha256"] != header_sha for value in (inventory, fonts)):
        raise ValueError("inventory and archive differ")
    candidates, startup = {}, {}
    for font in fonts["fonts"]:
        family = {"startup\\fonts\\crash.psf": "CrashFont",
                  "startup\\fonts\\arial.psf": "ArialFont"}.get(font["container"].lower())
        if family is None:
            raise ValueError("unclassified font owner")
        for page in font["pages"]:
            texture = int(page["texture_id"], 16)
            item = dict(material=int(page["material_id"], 16), family=family, bucket=26,
                        container=font["container"])
            candidates[texture] = startup[texture] = item
    for picture in inventory["pictures"]:
        family = "Hud" if picture["container"].lower() == "startup\\icons.psm" else "Picture"
        for sprite in picture["sprites"]:
            texture = int(sprite["texture_id"], 16)
            item = dict(material=int(sprite["material_id"], 16), family=family, bucket=24,
                        container=picture["container"])
            if texture in candidates and candidates[texture] != item:
                raise ValueError("different UI owners share a texture ID")
            candidates[texture] = item
            if family == "Hud":
                startup[texture] = item
    wanted = set(candidates) | {entry["material"] for entry in candidates.values()}
    references, hits = collections.defaultdict(list), collections.defaultdict(list)
    classes = collections.Counter()
    materials = 0
    with (args.archive / "CRASH.BD").open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for name, start, size in archive_index(index):
                ext = Path(name).suffix.lower()
                if ext == ".ptc":
                    texture, mid = struct.unpack_from("<II", data, start)
                    at = start + 8 + u32(data, start + 8) + 4
                    _, _, shaders = material(data, at, start + size - at)
                    if texture in candidates or mid in wanted or any(
                            shader["texture"] in candidates for shader in shaders):
                        raise ValueError("particle page consumes a candidate UI owner")
                    continue
                if ext not in (".sm2", ".rm2"):
                    continue
                root = section(data, start, size, 1)
                graphic_kind = 6 if ext == ".sm2" else 11
                graphics = section(data, *root[graphic_kind], 1)
                for mid, (at, length) in section(data, *graphics[1], 3).items():
                    programs, bucket, shaders = material(data, at, length)
                    materials += 1
                    for shader in shaders:
                        classes[shader["type"]] += 1
                        texture = shader["texture"]
                        if texture in candidates:
                            owner = candidates[texture]
                            if (mid, programs, bucket, len(shaders), shader["type"]) != (
                                    owner["material"], 0x10000000, owner["bucket"], 1, 13):
                                raise ValueError(f"retained world material consumes UI texture {texture:x}")
                            references[texture].append(dict(container=name, material=f"{mid:08x}"))
                # Registration directories/material records have already been
                # classified. All other model, particle, script/chunk payloads
                # must not be presumed free of candidate IDs.
                spans = [(f"graphics/{kind}", at, length) for kind, (at, length) in graphics.items()
                         if kind not in (0, 1)]
                spans += [(f"chunk/{kind}", at, length) for kind, (at, length) in root.items()
                          if kind != graphic_kind]
                for kind, at, length in spans:
                    for word, offset in raw_hits(data, at, length, wanted):
                        hits[word].append(dict(container=name, section=kind, offset=offset))
    for texture, owner in startup.items():
        if hits.get(texture) or hits.get(owner["material"]):
            raise ValueError(f"startup owner {texture:x} has an unresolved foreign reference")
        if {ref["container"] for ref in references[texture]} != {"Startup\\Default.rm2"}:
            raise ValueError("startup shared registration scope changed")
    catalogue(args.catalogue, startup, header_sha)
    selectable, deferred = [], []
    for texture, owner in sorted(candidates.items()):
        row = dict(texture=f"{texture:08x}", material=f"{owner['material']:08x}",
                   family=owner["family"], container=owner["container"],
                   shared_material_records=references[texture])
        unresolved = hits.get(texture, []) + hits.get(owner["material"], [])
        if unresolved:
            row["unresolved_raw_occurrences"] = unresolved
            deferred.append(row)
        else:
            selectable.append(row)
    report = dict(archive_header_sha256=header_sha, materials=materials,
                  shaders=sum(classes.values()), shader_classes=dict(classes),
                  startup_count=len(startup), selectable=selectable, deferred=deferred,
                  scope="static known asset graph; source acquisition/caller and live owner rules remain required")
    (args.out / "owners.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{materials} materials / {sum(classes.values())} shaders; {len(startup)} checked startup owners")
    print(f"{len(selectable)} UI candidates without foreign raw references; "
          f"{len(deferred)} conservatively deferred")


if __name__ == "__main__":
    main()
