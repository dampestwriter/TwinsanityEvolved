"""Read the title's declared texture containers from the retail archive, without play.

Spec (read-only, no copied code): TR chunkfiles.cpp (SM2 6 / RM2 11),
graphicstables/readers (graphics kind 0), font.cpp, renderer/particles.cpp,
Shape2D::Read, OLEG::ReadPicture and GameController's icon load.
Raw assets remain in the explicitly selected output directory, never in git.
PSM files contain source-declared runs of 1 title, 8 tiles or 40 HUD icons.
"""
import argparse
import collections
import hashlib
import json
import mmap
from pathlib import Path
import struct


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def archive_index(data):
    at = 4
    while at < len(data):
        length = u32(data, at)
        at += 4
        if not 0 < length <= 1024 or at + length + 8 > len(data):
            raise ValueError("invalid archive name extent")
        name = data[at:at + length].decode("ascii")
        at += length
        start, size = struct.unpack_from("<II", data, at)
        at += 8
        yield name, start, size


def section(data, start, size, expected):
    if size < 12 or start + size > len(data):
        raise ValueError("invalid section extent")
    version, count, _ = struct.unpack_from("<III", data, start)
    if version != 0x10000 + expected or 12 + count * 12 > size:
        raise ValueError("invalid section kind or count")
    records = {}
    for n in range(count):
        offset, length, key = struct.unpack_from("<III", data, start + 12 + n * 12)
        if offset < 12 + count * 12 or offset + length > size or key in records:
            raise ValueError("invalid section item")
        records[key] = start + offset, length
    return records


def font_material_end(data, at):
    """The source's base-only, unanimated type-D font material record."""
    name_size = u32(data, at + 12)
    at += 16 + name_size
    count = u32(data, at)
    at += 4
    if count > 4:
        raise ValueError("font material shader count")
    for _ in range(count):
        kind = u32(data, at)
        at += 4
        if kind != 13 or data[at + 29] or u32(data, at + 86) != kind:
            raise ValueError("unclassified font shader; extend source grammar")
        at += 90
    return at


def picture_material(data, at, limit):
    """Source base Shader reader: only the actually declared, unanimated UI type."""
    begin = at
    if at + 20 > limit:
        raise ValueError("truncated picture material")
    programs, bucket, name_size = struct.unpack_from("<QII", data, at)
    at += 16 + name_size
    if at + 4 > limit or u32(data, at) != 1:
        raise ValueError("picture material shader count")
    at += 4
    if at + 94 > limit or u32(data, at) != 13:
        raise ValueError("unclassified picture shader")
    at += 4
    if data[at + 29] or u32(data, at + 86) != 13:
        raise ValueError("unclassified picture shader animation/type")
    settings = dict(zip(("blend", "gouraud", "textured", "stq", "fog", "context", "linear"),
                        (data[at + n] for n in (0, 10, 11, 12, 13, 14, 23))))
    k, l = struct.unpack_from("<hh", data, at + 30)
    metadata = dict(programs=f"{programs:016x}", bucket=bucket, type=13, settings=settings,
                    lod_bias=k, lod_shift=l, texture_id=f"{u32(data, at + 82):08x}",
                    sha256=hashlib.sha256(data[begin:at + 90]).hexdigest())
    return at + 90, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = (args.archive / "CRASH.BH").read_bytes()
    formats, shapes, containers = collections.Counter(), collections.Counter(), collections.Counter()
    unique, appearances, pictures, ui_files = {}, [], [], {}

    def record(data, at, size, name, key, kind):
        blob = bytes(data[at:at + size])
        if len(blob) < 228 or u32(blob, 0) + 4 != len(blob):
            raise ValueError(f"texture size in {name}")
        digest = hashlib.sha256(blob).hexdigest()
        width, height = struct.unpack_from("<HH", blob, 8)
        levels, pixel, storage, alpha = blob[12:16]
        fmt = f"{pixel:02x}/{storage:02x}"
        shape = f"{fmt}:{width},{height}:{levels}"
        formats[fmt] += 1
        shapes[shape] += 1
        containers[kind] += 1
        file = f"texture_{digest}.bin"
        if digest not in unique:
            (args.out / file).write_bytes(blob)
            unique[digest] = dict(file=file, sha256=digest, size=size, format=fmt,
                                  width_power=width, height_power=height, levels=levels, alpha=alpha)
        appearances.append(dict(container=name, kind=kind, id=f"{key:08x}",
                                offset=at, size=size, sha256=digest))
        if kind == ".psm":
            ui_files[file] = None

    with (args.archive / "CRASH.BD").open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for name, start, size in archive_index(index):
                if start + size > len(data):
                    raise ValueError("archive item exceeds data")
                ext = Path(name).suffix.lower()
                if ext in (".sm2", ".rm2"):
                    root = section(data, start, size, 1)
                    graphics = section(data, *root[6 if ext == ".sm2" else 11], 1)
                    for key, (at, length) in section(data, *graphics[0], 3).items():
                        record(data, at, length, name, key, ext)
                elif ext == ".ptc":
                    # ReadParticlePageData(fromStream=1): texture id, material
                    # id, texture record, material; later particle data is not a texture.
                    record(data, start + 8, u32(data, start + 8) + 4, name, u32(data, start), ext)
                elif ext == ".psf":
                    count = u32(data, start)
                    at = start + 4
                    if count > 4:
                        raise ValueError("font page count")
                    for _ in range(count):
                        key = u32(data, at)
                        at += 8
                        length = u32(data, at) + 4
                        if at + length > start + size:
                            raise ValueError("font texture extent")
                        record(data, at, length, name, key, ext)
                        at = font_material_end(data, at + length)
                    if at + 8 > start + size or at + 8 + u32(data, at) * 16 != start + size:
                        raise ValueError("font glyph extent")
                elif ext == ".psm":
                    # No generic container/packet scanner: source Shape2D::Read
                    # consumes two IDs, one TextureRead record and one material.
                    at, sprites = start, []
                    while at < start + size:
                        if at + 12 > start + size:
                            raise ValueError("truncated picture pair")
                        texture, material = struct.unpack_from("<II", data, at)
                        at += 8
                        length = u32(data, at) + 4
                        if length < 228 or at + length > start + size:
                            raise ValueError("picture texture extent")
                        record(data, at, length, name, texture, ext)
                        at, shader = picture_material(data, at + length, start + size)
                        if shader["texture_id"] != f"{texture:08x}":
                            raise ValueError("picture material's different texture owner")
                        sprites.append(dict(texture_id=f"{texture:08x}", material_id=f"{material:08x}",
                                            material=shader))
                    expected = 40 if name.lower() == r"startup\icons.psm" else (
                        1 if name.lower().startswith("language\\titles\\") else 8)
                    if len(sprites) != expected:
                        raise ValueError(f"picture source count in {name}: {len(sprites)} != {expected}")
                    pictures.append(dict(container=name, sprites=sprites))
    report = dict(source="retail archive; static source-declared container walk",
                  archive_header_sha256=hashlib.sha256(index).hexdigest(),
                  limitations=["runtime patches/foreign assets not inferred from this retail inventory"],
                  formats=dict(formats), shapes=dict(shapes), containers=dict(containers),
                  unique=list(unique.values()), appearances=appearances, pictures=pictures)
    (args.out / "textures.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.out / "assets.txt").write_text("".join(v["file"] + "\n" for v in unique.values()))
    (args.out / "ui_assets.txt").write_text("".join(file + "\n" for file in ui_files))
    print(f"{len(appearances)} appearances; {len(unique)} distinct original records")
    print("formats", dict(formats), "containers", dict(containers))


if __name__ == "__main__":
    main()
