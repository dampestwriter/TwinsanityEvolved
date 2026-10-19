#!/usr/bin/env python3
"""Extract the source-enumerated culling image from the user's NTSC ELF.

No captured program, expected result, OLD implementation or TR code is imported.
The generated header contains only original program input for the actual NEW
model. Deliberately reject an unreviewed ELF or changed upload layout.
"""

import argparse
import hashlib
from pathlib import Path
import struct


ELF_SHA256 = "8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f"
IMAGE_SHA256 = "06b3da264376fad791906515364af1703ba464edd62698253680c159f2564c35"


def image_from_elf(data):
    if hashlib.sha256(data).hexdigest() != ELF_SHA256:
        raise ValueError("unreviewed ELF: culling source/NTSC provenance must be rechecked")
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32")
    offset = struct.unpack_from("<I", data, 28)[0]
    stride, count = struct.unpack_from("<HH", data, 42)
    segments = [struct.unpack_from("<8I", data, offset + i * stride) for i in range(count)]

    def read(address, length):
        for kind, file_offset, virtual, _, size, *_ in segments:
            if kind == 1 and virtual <= address and address + length <= virtual + size:
                start = file_offset + address - virtual
                return data[start:start + length]
        raise ValueError(f"unmapped source address {address:#x}")

    # NEW initializer 0x2b17f0 supplies this culling chain. The reviewed chain
    # consists of two MPG uploads, not a general DMA/VIF implementation.
    if struct.unpack("<4I", read(0x2E4B30, 16)) != (0x100000D0, 0, 0, 0x4A000000):
        raise ValueError("first culling upload changed")
    if struct.unpack("<2I", read(0x2E5340, 8)) != (0, 0x4A9F0100):
        raise ValueError("second culling upload changed")
    image = read(0x2E4B40, 2048) + read(0x2E5348, 1272) + bytes(776)
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("culling image digest changed")
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    image = image_from_elf(args.elf.read_bytes())
    lines = [", ".join(f"0x{b:02x}" for b in image[i:i + 16]) for i in range(0, len(image), 16)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "// Original source input extracted from the reviewed NTSC ELF; no expected output.\n"
        "#pragma once\n#include <array>\n#include <cstdint>\n"
        "namespace math_culling_oracle {\n"
        f'inline constexpr char kElfDigest[] = "{ELF_SHA256}";\n'
        f'inline constexpr char kImageDigest[] = "{IMAGE_SHA256}";\n'
        "inline constexpr std::array<uint8_t, 4096> kImage = {{\n    "
        + ",\n    ".join(lines) + "\n}};\n}\n"
    )


if __name__ == "__main__":
    main()
