"""Enumerate authored sky resources from a user's retail archive, without play.

The readonly decomp's archive/readers/graphicstables/resources/shaderclasses
describe these file containers. This tool does not use a captured VU image,
runtime absence, or an inferred address alias as coverage evidence. Extracted
assets remain outside the repository; the JSON records their hashes and refs.
"""
import argparse
import hashlib
import json
import mmap
import struct
from pathlib import Path


def words(data):
    if len(data) % 4:
        raise ValueError("unaligned word record")
    return struct.unpack("<" + "I" * (len(data) // 4), data)


def section(data, start, size, kind):
    if size < 12 or start + size > len(data):
        raise ValueError("section extent")
    fmt, count, _ = struct.unpack_from("<III", data, start)
    if fmt != 0x10000 + kind or 12 + count * 12 > size:
        raise ValueError("section format/table")
    result = {}
    for index in range(count):
        offset, length, key = struct.unpack_from("<III", data, start + 12 + 12 * index)
        if offset + length > size or key in result:
            raise ValueError("section item")
        result[key] = start + offset, length
    return result


def files(header):
    if len(header) < 4:
        raise ValueError("archive header")
    offset = 4
    while offset < len(header):
        length, = struct.unpack_from("<I", header, offset)
        offset += 4
        if length > 1024 or offset + length + 8 > len(header):
            raise ValueError("archive entry")
        name = header[offset:offset + length].decode("ascii")
        offset += length
        start, size = struct.unpack_from("<II", header, offset)
        offset += 8
        yield name, start, size


FIELDS = (
    "blends preset alpha_test alpha_method alpha_reference alpha_fail "
    "destination_test destination_mode depth_test unused23 gouraud textured stq "
    "fog second_context u_scroll v_scroll own_alpha alpha_a alpha_b alpha_c "
    "alpha_d alpha_fix linear no_fba unused57 anti_aliased no_depth_writes "
    "animated_colour has_animation"
).split()


def material(data):
    key, bucket, length = struct.unpack_from("<QII", data)
    offset = 16 + length
    name = data[16:offset].rstrip(b"\0").decode("ascii")
    count, = struct.unpack_from("<I", data, offset)
    offset += 4
    shaders = []
    for _ in range(count):
        shader_type, = struct.unpack_from("<I", data, offset)
        offset += 4
        settings = dict(zip(FIELDS, data[offset:offset + 30]))
        offset += 30
        lod = struct.unpack_from("<hh", data, offset)
        offset += 4 + 16  # tool leftovers
        colour = struct.unpack_from("<4f", data, offset)
        offset += 16
        scroll = struct.unpack_from("<4f", data, offset)
        offset += 16
        texture, repeated_type = struct.unpack_from("<II", data, offset)
        offset += 8
        if settings["has_animation"] or shader_type != 10 or repeated_type != 10:
            raise ValueError("new sky shader family: extend the source audit")
        shaders.append(dict(type=shader_type, settings=settings, lod=lod,
                            colour=colour, scroll=scroll, texture=f"{texture:08x}"))
    if offset != len(data):
        raise ValueError("unclassified sky material bytes")
    return dict(key=f"{key:016x}", bucket=bucket, name=name, shaders=shaders)


def model(data):
    count, = struct.unpack_from("<I", data)
    offset, parts = 4, []
    for _ in range(count):
        vertices, length = struct.unpack_from("<II", data, offset)
        offset += 8
        asset = data[offset:offset + length]
        if len(asset) != length:
            raise ValueError("model payload extent")
        parts.append(dict(vertices=vertices, offset=offset, size=length,
                          sha256=hashlib.sha256(asset).hexdigest()))
        offset += length
        extra, = struct.unpack_from("<I", data, offset)
        offset += 4 + extra
    if offset != len(data):
        raise ValueError("unclassified model bytes")
    return dict(parts=parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True,
                        help="directory containing CRASH.BH and CRASH.BD")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    header = (args.archive / "CRASH.BH").read_bytes()
    inventory = {kind: {} for kind in (0, 1, 2, 6, 8)}
    appearances = []
    with (args.archive / "CRASH.BD").open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for name, start, size in files(header):
                if not name.lower().endswith(".sm2"):
                    continue
                root = section(data, start, size, 1)
                if 6 not in root:
                    continue
                # Static chunk graphics section. RM2's other nine-item
                # sections are NOT Graphics merely because their count is nine.
                graphics = section(data, *root[6], 1)
                for kind, resources in inventory.items():
                    for key, (at, length) in section(data, *graphics[kind], 3).items():
                        blob = bytes(data[at:at + length])
                        digest = hashlib.sha256(blob).hexdigest()
                        resources.setdefault(key, {})[digest] = blob
                        if kind == 8:
                            appearances.append(dict(file=name, sky=f"{key:08x}",
                                                    offset=at, size=length, sha256=digest))

    selected = {kind: set() for kind in inventory}
    selected[8] = set(inventory[8])
    metadata = {}

    def variants(kind):
        for key in sorted(selected[kind]):
            for digest, blob in sorted(inventory[kind][key].items()):
                yield key, digest, blob

    for sky, digest, blob in variants(8):
        values = words(blob)
        if values[0] != 0x5000 or values[1] != len(values) - 2:
            raise ValueError("sky record")
        selected[6].update(values[2:])
        metadata[8, sky, digest] = dict(meshes=[f"{item:08x}" for item in values[2:]])
    for mesh, digest, blob in variants(6):
        values = words(blob)
        if values[1] != len(values) - 3:
            raise ValueError("mesh record")
        selected[1].update(values[2:-1])
        selected[2].add(values[-1])
        metadata[6, mesh, digest] = dict(materials=[f"{item:08x}" for item in values[2:-1]],
                                        model=f"{values[-1]:08x}")
    for key, digest, blob in variants(1):
        info = material(blob)
        selected[0].update(int(shader["texture"], 16) for shader in info["shaders"])
        metadata[1, key, digest] = info
    for key, digest, blob in variants(2):
        metadata[2, key, digest] = model(blob)
    for key, digest, blob in variants(0):
        # TextureRead: byte size followed by the 0x60-byte texture header.
        size, = struct.unpack_from("<I", blob)
        width, height = struct.unpack_from("<HH", blob, 8)
        levels, psm, upload, alpha = struct.unpack_from("<4B", blob, 12)
        metadata[0, key, digest] = dict(declared_size=size, width_power=width, height_power=height,
                                       levels=levels, format=psm, upload_format=upload, alpha=alpha)

    names = {0: "texture", 1: "material", 2: "model", 6: "mesh", 8: "sky"}
    report = dict(archive_header_sha256=hashlib.sha256(header).hexdigest(),
                  source="authored archive; static .sm2 graphics section 6",
                  appearances=appearances, resources={})
    for kind, keys in selected.items():
        name = names[kind]
        records = []
        for key, digest, blob in variants(kind):
            file = f"{name}_{key:08x}_{digest[:16]}.bin"
            (args.out / file).write_bytes(blob)
            records.append(dict(id=f"{key:08x}", file=file, size=len(blob),
                                sha256=digest, **metadata[kind, key, digest]))
        report["resources"][name] = records
    (args.out / "sky_assets.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(appearances)} sky appearances; " + ", ".join(
        f"{len(selected[kind])} {name}" for kind, name in names.items()))
    print("Coverage: authored static resources only; no producer or live retirement claim")


if __name__ == "__main__":
    main()
