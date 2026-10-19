"""Static authored rigid/placed family inventory from SM2/RM2 graphics."""
import argparse
import collections
import hashlib
import json
import mmap
import struct
from pathlib import Path

from audit_sky_assets import FIELDS, files, model, section, words


def material(blob, family):
    key, bucket, length = struct.unpack_from("<QII", blob)
    start = 16 + length
    count, shader = struct.unpack_from("<II", blob, start)
    if shader != family:
        return None
    settings = dict(zip(FIELDS, blob[start + 8:start + 38]))
    texture = struct.unpack_from("<I", blob, start + 8 + 30 + 4 + 16 + 16 + 16)[0]
    return dict(key=f"{key:016x}", bucket=bucket,
                name=blob[16:start].rstrip(b"\0").decode("ascii"), count=count,
                settings=settings, texture=f"{texture:08x}",
                complete_base_record=count == 1 and not settings["has_animation"] and
                    start + 4 + 94 == len(blob))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--shader-type", type=lambda value: int(value, 0), default=1)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    header = (args.archive / "CRASH.BH").read_bytes()
    locations = {kind: collections.defaultdict(list) for kind in (0, 1, 2, 3, 6)}
    occurrences = []
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        for filename, start, size in files(header):
            slot = {".sm2": 6, ".rm2": 11}.get(Path(filename).suffix.lower())
            if slot is None:
                continue
            root = section(data, start, size, 1)
            if slot not in root:
                continue
            graphics = section(data, *root[slot], 1)
            for kind, index in locations.items():
                for key, (at, length) in section(data, *graphics[kind], 3).items():
                    index[key].append((filename, at, length))
        selected = {kind: set() for kind in locations}
        metadata = {}

        def variants(kind, keys):
            for key in sorted(keys):
                seen = set()
                for filename, at, size in locations[kind][key]:
                    blob = bytes(data[at:at + size])
                    digest = hashlib.sha256(blob).hexdigest()
                    if digest in seen:
                        continue
                    seen.add(digest)
                    yield key, digest, blob

        for key, digest, blob in variants(1, locations[1]):
            info = material(blob, args.shader_type)
            if info is None:
                continue
            selected[1].add(key)
            metadata[1, key, digest] = info
            if info["texture"] != "00000000":
                selected[0].add(int(info["texture"], 16))
        for kind in (3, 6):
            for key, digest, blob in variants(kind, locations[kind]):
                values = words(blob)
                if len(values) < 3 or values[1] != len(values) - 3:
                    raise ValueError("unclassified rigid resource")
                materials = list(values[2:-1])
                if not any(item in selected[1] for item in materials):
                    continue
                selected[kind].add(key)
                selected[2].add(values[-1])
                metadata[kind, key, digest] = dict(materials=[f"{i:08x}" for i in materials],
                    model=f"{values[-1]:08x}", all_selected_ids=all(i in selected[1] for i in materials))
        for key, digest, blob in variants(2, selected[2]):
            metadata[2, key, digest] = model(blob)
        for key, digest, blob in variants(0, selected[0]):
            size = struct.unpack_from("<I", blob)[0]
            width, height = struct.unpack_from("<HH", blob, 8)
            levels, fmt, upload, alpha = struct.unpack_from("<4B", blob, 12)
            metadata[0, key, digest] = dict(declared_size=size, width_power=width, height_power=height,
                levels=levels, format=fmt, upload_format=upload, alpha=alpha)
        names = {0: "texture", 1: "material", 2: "model", 3: "rigid", 6: "mesh"}
        output = {}
        for kind, keys in selected.items():
            rows = []
            for key, digest, blob in variants(kind, keys):
                info = metadata.get((kind, key, digest))
                if info is None:
                    # An ID also occurs with a different shader family.
                    info = dict(unclassified_variant=True)
                filename = f"{names[kind]}_{key:08x}_{digest[:16]}.bin"
                (args.out / filename).write_bytes(blob)
                rows.append(dict(id=f"{key:08x}", sha256=digest, size=len(blob), file=filename, **info))
            output[names[kind]] = rows
        for key in sorted(selected[1]):
            for filename, at, size in locations[1][key]:
                occurrences.append(dict(file=filename, id=f"{key:08x}", offset=at, size=size))
    report = dict(archive_header_sha256=hashlib.sha256(header).hexdigest(),
        source="TR chunkfiles.cpp SM2 section 6 / RM2 section 11; Graphics kinds 0,1,2,3,6",
        shader_type=args.shader_type,
        scope="Selected-family materials and associated rigid/model/texture variants; mixed families explicitly unclosed",
        appearances=occurrences, resources=output)
    # Conservative ID association: one resource ID may have several authored
    # variants. This is a test candidate inventory, never a certificate that a
    # material has no skin/direct/other consumers. The runtime gate is stricter.
    eligible = {row["id"] for row in output["material"] if row.get("complete_base_record")}
    parts = collections.defaultdict(set)
    for kind in ("rigid", "mesh"):
        for row in output[kind]:
            for index, identifier in enumerate(row.get("materials", [])):
                if identifier in eligible:
                    parts[row["model"]].add(index)
    seen, manifest = set(), []
    for row in output["model"]:
        for index, part in enumerate(row["parts"]):
            if index not in parts[row["id"]] or part["sha256"] in seen:
                continue
            seen.add(part["sha256"])
            manifest.append(f'{row["file"]}\t{index}\n')
    (args.out / "parts.tsv").write_text("".join(manifest))
    report["distinct_candidate_parts"] = len(manifest)
    report["part_manifest_sha256"] = hashlib.sha256("".join(manifest).encode()).hexdigest()
    (args.out / "rigid_assets.json").write_text(json.dumps(report, indent=2) + "\n")
    for kind, rows in output.items():
        print(f"{kind}: {len({row['id'] for row in rows})} IDs, {len(rows)} serialized variants")


if __name__ == "__main__":
    main()
