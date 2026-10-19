"""Extract every statically declared retail type-0B model for the NEW oracle.

Read-only authority: TR game/shadows.cpp LoadShadowMeshes(false), graphics kind
6 records and renderer/shaders.cpp type 0B. No capture or observed-play set.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "textures"))
from audit_material_consumers import inventory

HEADER_SHA = "27fe74b6f1253954dc9c2bede794a8528df0b035852ce4e8213d8d08ad42f6a7"
DATA_SHA = "eeedcae83d4e0c3ff483fe9f8afa8d3822bd734027434a7a1abbb84273dd17d7"
MODELS = {
    0: (0xd8d37a1c, "767c850ddae7f14e06222c8c1446afef875e6ac98343df31c193a32981980b61"),
    1: (0x6bb7b765, "d8c7df776fe1a3eb8353e23f8310abedbfd45a620e652ec7662431267639163c"),
    2: (0xf37a152c, "0277cf0c53490d63f1545a9adc7dc7b533c5263bc8365222b44a4ecfc7bdad59"),
    3: (0x92c82e8b, "b8c68f58e8fb79ae5d7ed7b73961ef2ad7141644d50f88c8cd4b7f833fd2095f"),
    4: (0xb2da733d, "7e3facefd8c3bf4dcea576e7c277caed82dd6dec3576b4831ea77af05a55a3a0"),
    5: (0x09b44da5, "55c5f1ca52602997847c66ab41ff238f497c112b945284bbd170bd5b218fc7eb"),
    6: (0x2adc18fb, "df5ce146cae5ecdbdb9840780d0d0de602b238c824cea6963839bbb6365a40ed"),
    7: (0x231cda3b, "9c13006dbc3c12a53f753a69bc61f2b60c62b8a44c9eb98e38ee9f22fd70f66b"),
}


def fnv(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    header = (args.archive / "CRASH.BH").read_bytes()
    if hashlib.sha256(header).hexdigest() != HEADER_SHA:
        raise ValueError("unreviewed archive header")
    args.out.mkdir(parents=True, exist_ok=True)
    rows, report = [], []
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != DATA_SHA:
            raise ValueError("unreviewed archive data")
        materials, models, draws, _, _, _, _ = inventory(data, header)
        mids = {mid for mid, versions in materials.items() if any(
            s["type"] == 11 for v in versions.values() for s in v["record"]["shaders"])}
        selected = [d for d in draws if set(d["materials"]) & mids]
        if mids != {0x791fa4a8} or len(selected) != 8 or {d["id"] for d in selected} != set(MODELS):
            raise ValueError("type-0B material/consumer graph changed")
        for draw in sorted(selected, key=lambda d: d["id"]):
            shape = draw["id"]
            model, sha = MODELS[shape]
            if (draw["kind"] != 6 or draw["container"] != "Startup\\Default.rm2" or
                    draw["model"] != model or draw["materials"] != [0x791fa4a8] or
                    set(models[model]) != {sha}):
                raise ValueError("unreviewed shadow consumer")
            record = models[model][sha]
            if len(record["parts"]) != 1:
                raise ValueError("shadow model part count changed")
            part = record["parts"][0]
            payload = data[part["offset"]:part["offset"] + part["size"]]
            filename = f"{shape}-{model:08x}.bin"
            (args.out / filename).write_bytes(payload)
            rows.append(f'{shape} {part["vertices"]} {fnv(payload):016x} {filename}')
            report.append(dict(shape=shape, model=f"{model:08x}", model_sha256=sha,
                               part_sha256=hashlib.sha256(payload).hexdigest(), **part))
    (args.out / "shadows.txt").write_text("\n".join(rows) + "\n")
    (args.out / "shadows.json").write_text(json.dumps(dict(
        archive_header_sha256=HEADER_SHA, archive_data_sha256=DATA_SHA, models=report,
        scope="All eight default type-0B shapes; file-fallback caller disposition remains separate."),
        indent=2) + "\n")
    print(f"Verified complete type-0B asset graph: {len(report)} shapes, "
          f'{sum(p["vertices"] for p in report)} authored vertices')


if __name__ == "__main__":
    main()
