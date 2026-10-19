"""Enumerate the complete retail cloth material/geometry graph, without play.

TR renderer/{resources,models,shaderclasses,shadertypes,shaders}.cpp supplies
the file and consumer contracts. Original authored payloads are extracted
unchanged for the independent NEW program oracle, never committed.
"""
import argparse
import collections
import hashlib
import json
import math
import mmap
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "textures"))
from audit_material_consumers import inventory
from audit_texture_assets import archive_index, section
from audit_texture_owners import raw_hits
from audit_shadow_assets import HEADER_SHA, DATA_SHA, fnv


def prefixes(data, header, wanted):
    result, payloads = {}, {}
    for name, start, size in archive_index(header):
        ext = Path(name).suffix.lower()
        if ext not in (".rm2", ".sm2"):
            continue
        root = section(data, start, size, 1)
        graphics = section(data, *root[6 if ext == ".sm2" else 11], 1)
        if 1 not in graphics:
            continue
        for mid, (at, length) in section(data, *graphics[1], 3).items():
            if mid not in wanted:
                continue
            sha = hashlib.sha256(data[at:at + length]).hexdigest()
            name_size = struct.unpack_from("<I", data, at + 12)[0]
            count, kind = struct.unpack_from("<II", data, at + 16 + name_size)
            if count != 1:
                raise ValueError("cloth consumer has multiple material passes")
            if kind not in (0x17, 0x1a):
                continue  # Same ID, a different loaded ordinary-material version.
            values = struct.unpack_from("<if" + "f" * (1 if kind == 0x17 else 3),
                                        data, at + 24 + name_size)
            mode, speed, *amplitude = values
            if mode not in (0, 1, 2) or not all(math.isfinite(v) for v in values[1:]):
                raise ValueError("unclassified cloth update values")
            result[mid, sha] = dict(type=kind, mode=mode, speed=speed, amplitude=amplitude)
            payloads[mid, sha] = bytes(data[at:at + length])
    return result, payloads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    header = (args.archive / "CRASH.BH").read_bytes()
    if hashlib.sha256(header).hexdigest() != HEADER_SHA:
        raise ValueError("unreviewed archive header")
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != DATA_SHA:
            raise ValueError("unreviewed archive data")
        materials, models, draws, excluded, _, spans, _ = inventory(data, header)
        mids = {mid for mid, versions in materials.items() if any(
            s["type"] in (0x17, 0x1a) for v in versions.values() for s in v["record"]["shaders"])}
        params, material_payloads = prefixes(data, header, mids)
        selected = [d for d in draws if set(d["materials"]) & mids]
        if len(mids) != 50 or len(params) != 50 or len(selected) != 424:
            raise ValueError("complete cloth material/consumer graph changed")
        if any(excluded[mid] for mid in mids):
            raise ValueError("new non-mesh cloth consumer")
        hits = collections.defaultdict(list)
        for name, kind, at, size in spans:
            for mid, offset in raw_hits(data, at, size, mids):
                hits[mid].append(dict(container=name, section=kind, offset=offset))
        fixtures, consumers, shortened = {}, [], []
        for draw in sorted(selected, key=lambda d: (d["id"], d["sha256"], d["container"])):
            if draw["model"] not in models:
                raise ValueError("missing cloth model")
            for model_sha, model in sorted(models[draw["model"]].items()):
                if len(model["parts"]) != len(draw["materials"]):
                    # DrawnSubModels explicitly returns min(model part count,
                    # rigid material count). Surplus slots are not submitted.
                    shortened.append(dict(draw_id=f'{draw["id"]:08x}', model_sha256=model_sha,
                        parts=len(model["parts"]), materials=len(draw["materials"]),
                        drawn=min(len(model["parts"]), len(draw["materials"]))))
                for slot, (part, mid) in enumerate(zip(model["parts"], draw["materials"])):
                    if mid not in mids:
                        continue
                    payload = data[part["offset"]:part["offset"] + part["size"]]
                    sha = hashlib.sha256(payload).hexdigest()
                    # The graph deliberately includes every loaded version,
                    # not a guessed container or runtime-observed pairing.
                    for material_sha in sorted(materials[mid]):
                        values = params.get((mid, material_sha))
                        if not values:
                            continue
                        amplitude = values["amplitude"]
                        if len(amplitude) == 1:
                            amplitude = amplitude * 3
                        key = (values["type"], sha, tuple(amplitude))
                        if key not in fixtures:
                            fixtures[key] = dict(type=values["type"], sha256=sha,
                                amplitude=amplitude, payload=payload, **part)
                        consumers.append(dict(draw_id=f'{draw["id"]:08x}', kind=draw["kind"],
                            container=draw["container"], draw_sha256=draw["sha256"],
                            model_id=f'{draw["model"]:08x}', model_sha256=model_sha, slot=slot,
                            material_id=f"{mid:08x}", material_sha256=material_sha,
                            part_sha256=sha, type=values["type"]))
        report = dict(archive_header_sha256=HEADER_SHA, archive_data_sha256=DATA_SHA,
            material_ids=len(mids), material_versions=sum(len(materials[m]) for m in mids),
            cloth_material_versions=len(params), draw_appearances=len(selected),
            draw_kinds=dict(collections.Counter(d["kind"] for d in selected)),
            material_records=[dict(id=f"{mid:08x}", sha256=sha, **values,
                source=materials[mid][sha]) for (mid, sha), values in sorted(params.items())],
            other_versions=[dict(id=f"{mid:08x}", sha256=sha, source=record)
                for mid in sorted(mids) for sha, record in sorted(materials[mid].items())
                if (mid, sha) not in params],
            non_mesh_consumers={f"{m:08x}": sorted(excluded[m]) for m in sorted(mids) if excluded[m]},
            opaque_references={f"{m:08x}": rows for m, rows in sorted(hits.items())},
            consumers=consumers, shortened_source_draws=shortened, fixtures=[],
            scope="Complete archive, conservative cross-version mesh/material graph. "
                  "Only an exact loaded version is selectable. Opaque references remain visible; "
                  "geometry agreement alone does not retire material/texture/source producers.")
        args.out.mkdir(parents=True, exist_ok=True)
        lines = []
        for index, (_, fixture) in enumerate(sorted(fixtures.items())):
            payload = fixture.pop("payload")
            filename = f'{fixture["type"]:02x}-{fixture["sha256"]}.bin'
            (args.out / filename).write_bytes(payload)
            amplitudes = " ".join(repr(v) for v in fixture["amplitude"])
            lines.append(f'{fixture["type"]} {fixture["vertices"]} {fnv(payload):016x} '
                         f"{amplitudes} {filename}")
            report["fixtures"].append(dict(index=index, filename=filename, **fixture))
        manifest = "\n".join(lines) + "\n"
        report["manifest_sha256"] = hashlib.sha256(manifest.encode()).hexdigest()
        (args.out / "cloth.txt").write_text(manifest)
        material_lines = []
        for (mid, sha), payload in sorted(material_payloads.items()):
            filename = f"material-{mid:08x}-{sha}.bin"
            (args.out / filename).write_bytes(payload)
            material_lines.append(f"{mid:08x} {fnv(payload):016x} {filename}")
        material_manifest = "\n".join(material_lines) + "\n"
        report["material_manifest_sha256"] = hashlib.sha256(material_manifest.encode()).hexdigest()
        (args.out / "cloth-materials.txt").write_text(material_manifest)
        (args.out / "cloth.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Cloth: {len(mids)} IDs/{len(params)} cloth versions, {len(selected)} appearances, "
              f"{len(fixtures)} original part/parameter fixtures, {sum(map(len, hits.values()))} opaque hits")
        print("Draw kinds:", report["draw_kinds"])
        print("Modes:", dict(collections.Counter(v["mode"] for v in params.values())))
        print("Buckets:", dict(collections.Counter(
            materials[m][sha]["record"]["bucket"] for m, sha in params)))
        settings = collections.defaultdict(set)
        for m, sha in params:
            for key, value in materials[m][sha]["record"]["shaders"][0]["settings"].items():
                settings[key].add(value)
        print("Settings:", {key: sorted(values) for key, values in settings.items()})


if __name__ == "__main__":
    main()
