#!/usr/bin/env python3
"""Extract the statically identified standard animation image from the NTSC ELF."""
import argparse
import hashlib
from pathlib import Path
import struct

ELF_SHA256 = "8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f"
IMAGE_SHA256 = "4503744cf26b324a3e9b4f753e20997fec16a6d4d06d5ed6012c4e5ee47302f7"


def image_from_elf(data):
    if hashlib.sha256(data).hexdigest() != ELF_SHA256:
        raise ValueError("unreviewed ELF: standard animation source provenance must be checked")
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

    # NEW initializer 2b17f0 writes this chain into source set slot 1.
    # Exactly two reviewed uploads; this is not a transport implementation.
    if struct.unpack("<4I", read(0x2E5D90, 16)) != (0x100000DA, 0, 0, 0x4A000000):
        raise ValueError("first standard upload changed")
    if struct.unpack("<2I", read(0x2E65A0, 8)) != (0, 0x4AB30100):
        raise ValueError("second standard upload changed")
    image = read(0x2E5DA0, 2048) + read(0x2E65A8, 1432) + bytes(616)
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("standard animation image digest changed")
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    image = image_from_elf(args.elf.read_bytes())
    rows = [", ".join(f"0x{b:02x}" for b in image[i:i + 16]) for i in range(0, len(image), 16)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "// Actual original program input from reviewed ELF; no captured or expected output.\n"
        "#pragma once\n#include <array>\n#include <cstdint>\n"
        "namespace math_joint_oracle {\n"
        f'inline constexpr char kElfDigest[] = "{ELF_SHA256}";\n'
        f'inline constexpr char kImageDigest[] = "{IMAGE_SHA256}";\n'
        "inline constexpr std::array<uint8_t, 4096> kImage = {{\n    "
        + ",\n    ".join(rows) + "\n}};\n}\n")


if __name__ == "__main__":
    main()
