#!/usr/bin/env python3
"""Extract unchanged font glyph arrays and enumerate their source material contracts.

Read-only specification: TR game/font.cpp and renderer/shaderclasses.cpp.
Original bytes are written only to the explicitly chosen fixture directory.
The atlas pixels are independently covered by the TextureAssets byte oracle.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import mmap
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    module = Path(__file__).resolve().parent.parent / "textures/audit_texture_assets.py"
    spec = importlib.util.spec_from_file_location("texture_inventory", module)
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
    args.out.mkdir(parents=True, exist_ok=True)
    index = (args.archive / "CRASH.BH").read_bytes()
    fonts, manifest = [], []
    with (args.archive / "CRASH.BD").open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for name, start, size in inventory.archive_index(index):
                if Path(name).suffix.lower() != ".psf":
                    continue
                if start + size > len(data) or size < 12:
                    raise ValueError("font extent")
                count = inventory.u32(data, start)
                if not 1 <= count <= 3:
                    raise ValueError("font page count")
                pages, at = [], start + 4
                for page in range(count):
                    texture, material = struct.unpack_from("<II", data, at)
                    at += 8
                    length = inventory.u32(data, at) + 4
                    if length < 228 or at + length > start + size:
                        raise ValueError("font texture extent")
                    texture_bytes = data[at:at + length]
                    at += length
                    end = inventory.font_material_end(data, at)
                    programs, bucket, name_length = struct.unpack_from("<QII", data, at)
                    shaders = inventory.u32(data, at + 16 + name_length)
                    if end > start + size or (programs, bucket, shaders) != (0x10000000, 26, 1):
                        raise ValueError("undeclared font material contract")
                    pages.append(dict(page=page, texture_id=f"{texture:08x}", material_id=f"{material:08x}",
                                      programs=f"{programs:016x}", bucket=bucket, shaders=shaders,
                                      shader_type=13, texture_sha256=hashlib.sha256(texture_bytes).hexdigest(),
                                      material_sha256=hashlib.sha256(data[at:end]).hexdigest()))
                    at = end
                glyph_count, first = struct.unpack_from("<II", data, at)
                if not 1 <= glyph_count <= 256 or at + 8 + glyph_count * 16 != start + size:
                    raise ValueError("font glyph extent")
                glyphs = data[at + 8:at + 8 + glyph_count * 16]
                glyph_pages = collections.Counter(glyphs[n * 16] & 3 for n in range(glyph_count))
                if any(page > count for page in glyph_pages):
                    raise ValueError("glyph refers to absent page")
                digest = hashlib.sha256(glyphs).hexdigest()
                file = f"glyphs_{digest}.bin"
                (args.out / file).write_bytes(glyphs)
                manifest.append(f"{count} {first} {glyph_count} {file}\n")
                fonts.append(dict(container=name, offset=start, size=size,
                                  sha256=hashlib.sha256(data[start:start + size]).hexdigest(),
                                  first_character=first, glyph_count=glyph_count,
                                  glyph_pages=dict(glyph_pages), glyph_sha256=digest, file=file, pages=pages))
    if not fonts:
        raise ValueError("no source font assets")
    report = dict(source="static retail archive PSF grammar; original glyph bytes",
                  archive_header_sha256=hashlib.sha256(index).hexdigest(), fonts=fonts)
    (args.out / "fonts.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.out / "fonts.txt").write_text("".join(manifest))
    print(f"{len(fonts)} fonts; {sum(len(font['pages']) for font in fonts)} atlas pages; "
          f"{sum(font['glyph_count'] for font in fonts)} original glyphs")


if __name__ == "__main__":
    main()
