#!/usr/bin/env python3
"""Enumerate the complete retail material graph; refuse unclosed consumers.

Read-only schema authority: TR renderer/resources.cpp, models.cpp,
game/{shadows,oleg,shapes}.cpp. No game execution or packet interpretation.
The original asset bytes stay outside the repository.
"""
import argparse
import collections
import hashlib
import json
import mmap
from pathlib import Path
import struct

from audit_texture_assets import archive_index, section, u32
from audit_texture_owners import raw_hits
from material_graph import close, texture_scope
import material_records as records


def digest(data, at, size):
    return hashlib.sha256(data[at:at + size]).hexdigest()


def inventory(data, index):
    materials, models = collections.defaultdict(dict), collections.defaultdict(dict)
    excluded, textures = collections.defaultdict(set), set()
    draws, skies, spans = [], set(), []
    counts = collections.Counter()
    all_meshes = []
    for name, start, size in archive_index(index):
        ext = Path(name).suffix.lower()
        if ext not in (".sm2", ".rm2"):
            if ext == ".ptc":
                tid, mid = struct.unpack_from("<II", data, start)
                at = start + 12 + u32(data, start + 8)
                parsed, end = records.material(data, at, start + size - at)
                materials[mid][digest(data, at, end - at)] = dict(record=parsed, container=name)
                excluded[mid].add("particle")
                textures.add(tid)
            elif ext in (".psm", ".psf"):
                # Exact complete source grammar is checked by the texture/font
                # inventories. Here every material/texture pair is revisited.
                at = start
                if ext == ".psm":
                    count = 40 if name.lower() == "startup\\icons.psm" else (
                        1 if name.lower().startswith("language\\titles\\") else 8)
                else:
                    count = u32(data, at)
                    at += 4
                for _ in range(count):
                    tid, mid = struct.unpack_from("<II", data, at)
                    at += 8
                    at += 4 + u32(data, at)
                    begin = at
                    parsed, at = records.material(data, at, start + size - at, complete=False)
                    materials[mid][digest(data, begin, at - begin)] = dict(record=parsed, container=name)
                    excluded[mid].add("sprite" if ext == ".psm" else "font")
                    textures.add(tid)
                if ext == ".psm" and at != start + size:
                    raise ValueError(f"picture tail {name}")
                if ext == ".psf":
                    glyphs, first = struct.unpack_from("<II", data, at)
                    if first > 0x10ffff or not 1 <= glyphs <= 256 or at + 8 + glyphs * 16 != start + size:
                        raise ValueError(f"font glyph extent {name}")
            else:
                spans.append((name, "other-file", start, size))
            continue
        root = section(data, start, size, 1)
        graphics_kind = 6 if ext == ".sm2" else 11
        graphics = section(data, *root[graphics_kind], 1)
        spans.extend((name, f"chunk/{k}", *span) for k, span in root.items() if k != graphics_kind)
        for kind, span in graphics.items():
            if kind == 0:
                continue  # source texture inventory checks all of these
            for rid, (at, length) in section(data, *span, 3).items():
                counts[kind] += 1
                sha = digest(data, at, length)
                if kind == 1:
                    parsed, _ = records.material(data, at, length)
                    materials[rid][sha] = dict(record=parsed, container=name)
                elif kind == 2:
                    parts = records.model_parts(data, at, length)
                    models[rid][sha] = dict(parts=parts, container=name, offset=at, size=length)
                elif kind in (3, 6):
                    parsed = records.rigid(data, at, length)
                    draw = dict(id=rid, kind=kind, sha256=sha, container=name, **parsed)
                    draws.append(draw)
                    if kind == 3 and draw["materials"]:
                        # OLEG::SetHudIcon may use any OGI's first rigid material.
                        # Script/model-slot closure is not inferred from play.
                        excluded[draw["materials"][0]].add("potential-borrowed-hud")
                    if kind == 6:
                        all_meshes.append(draw)
                        if rid < 8:  # LoadShadowMeshes(false), all ShadowShapeCount
                            for mid in draw["materials"]:
                                excluded[mid].add("shadow-default")
                elif kind in (4, 5):
                    mids = (records.skin if kind == 4 else records.blend)(data, at, length)
                    for mid in mids:
                        excluded[mid].add("skin" if kind == 4 else "blend-skin")
                elif kind in (7, 8):
                    mids = records.mesh_references(data, at, length, kind)
                    if kind == 8:
                        skies.update(mids)
                else:
                    raise ValueError(f"unclassified graphics kind {kind}")
    for draw in all_meshes:
        if draw["id"] in skies:
            for mid in draw["materials"]:
                excluded[mid].add("sky")
    return materials, models, draws, excluded, textures, spans, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--textures", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--qualification", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = (args.archive / "CRASH.BH").read_bytes()
    header_sha = hashlib.sha256(index).hexdigest()
    ti = json.loads(args.textures.read_text())
    if ti["archive_header_sha256"] != header_sha:
        raise ValueError("texture inventory archive mismatch")
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        data_sha = hashlib.sha256(data).hexdigest()
        materials, models, draws, excluded, foreign, spans, counts = inventory(data, index)
        wanted = {mid for mid, variants in materials.items()
                  if all(records.type01(v["record"]) for v in variants.values())}
        wanted_tex = {s["texture"] for mid in wanted for v in materials[mid].values()
                      for s in v["record"]["shaders"]}
        hits = collections.defaultdict(list)
        for name, kind, at, size in spans:
            for word, offset in raw_hits(data, at, size, wanted | wanted_tex):
                if word in wanted:
                    excluded[word].add("opaque-reference")
                if word in wanted_tex:
                    foreign.add(word)
                if len(hits[word]) < 8:
                    hits[word].append(dict(container=name, section=kind, offset=offset))
    variants = {}
    for versions in models.values():
        variants.update(versions)
    # Qualification manifest includes ALL model parts and is tied to complete
    # records, avoiding a favorable part sample from an otherwise mixed model.
    lines = []
    for sha, v in sorted(variants.items()):
        lines.append(f"{sha} {len(v['parts'])} " + " ".join(
            f"{p['offset']} {p['size']} {p['vertices']}" for p in v["parts"]))
    manifest = "\n".join(lines) + "\n"
    manifest_sha = hashlib.sha256(manifest.encode()).hexdigest()
    (args.out / "models.txt").write_text(manifest)
    qualified = set()
    if args.qualification:
        lines = args.qualification.read_text().splitlines()
        if lines.pop(0) != manifest_sha:
            raise ValueError("geometry qualification is for a different manifest")
        for line in lines:
            sha, accepted = line.split()
            if sha not in variants or accepted not in ("0", "1"):
                raise ValueError("invalid geometry qualification row")
            if accepted == "1":
                qualified.add(sha)
        if len(lines) != len(variants) or len({l.split()[0] for l in lines}) != len(variants):
            raise ValueError("incomplete geometry qualification")
    texture_ids = {int(v["id"], 16) for v in ti["appearances"]}
    for mid, versions in materials.items():
        if any(s["texture"] not in texture_ids for v in versions.values()
               for s in v["record"]["shaders"] if s["texture"]):
            excluded[mid].add("missing-texture")
    owned, reasons = close(materials, draws, models, excluded, qualified)
    texture_owners = texture_scope(materials, owned, foreign)
    accepted_draws = [draw for draw in draws if draw["materials"] and set(draw["materials"]) <= owned]
    accepted = []
    for mid in sorted(owned):
        versions = materials[mid]
        tid = next(iter(versions.values()))["record"]["shaders"][0]["texture"]
        accepted.append(dict(material=f"{mid:08x}", texture=f"{tid:08x}",
                             material_sha256=sorted(versions),
                             texture_exclusive=texture_owners[tid]))
    report = dict(archive_header_sha256=header_sha, archive_data_sha256=data_sha,
                  models_manifest_sha256=manifest_sha,
                  geometry_qualified=len(qualified), appearances=dict(counts),
                  material_ids=len(materials), material_variants=sum(map(len, materials.values())),
                  model_ids=len(models), model_variants=len(variants),
                  draw_appearances=len(draws), type01_candidates=len(wanted),
                  accepted_draw_appearances=dict(collections.Counter(
                      "rigid" if d["kind"] == 3 else "placed" for d in accepted_draws)),
                  accepted_draw_ids={label: len({d["id"] for d in accepted_draws if d["kind"] == kind})
                                     for label, kind in (("rigid", 3), ("placed", 6))},
                  certified=accepted,
                  reason_counts=dict(collections.Counter(r for rs in reasons.values() for r in rs)),
                  refused={f"{mid:08x}": sorted(rs) for mid, rs in sorted(reasons.items()) if rs},
                  raw_hits={f"{mid:08x}": rows for mid, rows in sorted(hits.items())},
                  scope="Known retail files and source consumer graph; unknown/replaced records are unowned. "
                        "No renderer/environment/readiness or texture retirement is implied.")
    (args.out / "materials.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(materials)} material IDs, {len(variants)} model variants, "
          f"{len(wanted)} type01 candidates, {len(owned)} certified")
    print(json.dumps(report["reason_counts"], sort_keys=True))


if __name__ == "__main__":
    main()
