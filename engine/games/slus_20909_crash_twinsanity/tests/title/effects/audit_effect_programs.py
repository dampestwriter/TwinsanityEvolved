"""Source checked NTSC special program inventory; static ELF bytes, never a run."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recomp"))
from audit_mesh_boundaries import ELF_SHA, sections

PROGRAMS = {
    "01": (0x1DBBC0, 0x2DF380, 0x2EBFE8),
    "0b": (0x1DC068, 0x2E1710, 0x2EBFFC),
    "17": (0x1DB4A0, 0x2DD400, 0x2EBFD8),
    "1a": (0x1DB1F0, 0x2DCC30, 0x2EBFD4),
    "18": (0x1DBF38, 0x2E05E0, 0x2EBFF4),
    "1c": (0x1DA808, 0x2DADF0, 0x2EBFBC),
}


def body(text, address):
    start = text.index(f"void FUN_{address:08x}(")
    return text[start:text.index("\n// Function:", start)], text[:start].count("\n") + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--decoder", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    elf = args.elf.read_bytes()
    if hashlib.sha256(elf).hexdigest() != ELF_SHA:
        raise ValueError("unreviewed executable")
    blocks = list(sections(elf))

    def data(address, size):
        for _, base, content in blocks:
            if base <= address and address + size <= base + len(content):
                return content[address-base:address-base+size]
        raise ValueError(f"ELF range absent: {address:x}+{size:x}")

    source = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_014.cpp")
    text = (args.root / source).read_text()
    header = (args.root / "engine/vu/vu_decoder.h").read_text()
    names = {kind: re.findall(rf"\bVUK_{kind}_(\w+)\b", header) for kind in ("U", "L")}
    rows = []
    args.out.mkdir(parents=True, exist_ok=True)
    for family, (entry, code, size_address) in PROGRAMS.items():
        generated, line = body(text, entry)
        # Checked registration ABI: r8 = image base; r4 = size-table page;
        # signed halfword at r4+offset is published with rounded-even size.
        match = re.search(r"r\[8\]\.SL\[0\]\) \+ (-?0x[0-9a-f]+)", generated)
        size_match = re.search(r"r\[4\]\.UL\[0\]\) \+ (-?0x[0-9a-f]+)", generated)
        if (not match or 0x2E0000 + int(match[1], 16) != code or not size_match or
                0x2F0000 + int(size_match[1], 16) != size_address or
                "r[8].SD[0] = static_cast<int32_t>(0x2e0000)" not in generated or
                "r[4].SD[0] = static_cast<int32_t>(0x2f0000)" not in generated):
            raise ValueError(f"changed registration body for {family}")
        count = struct.unpack("<h", data(size_address, 2))[0]
        if count <= 0 or count > 1024:
            raise ValueError("invalid static program size")
        image = data(code, count * 8)
        binary = args.out / f"{family}.bin"
        binary.write_bytes(image)
        decoded = subprocess.run([str(args.decoder), str(binary)], check=True,
                                 capture_output=True, text=True).stdout
        listing = []
        for record in decoded.splitlines():
            fields = record.split()
            upper, lower = names["U"][int(fields[3])], names["L"][int(fields[4])]
            listing.append(f'{int(fields[0]):03x} {fields[1]} {fields[2]} '
                           f'{upper:10} fd/fs/ft/mask/bc={",".join(fields[5:10])} '
                           f'{lower:8} fd/fs/ft/mask/fsf/imm11/imm15={",".join(fields[10:17])} '
                           f'I={fields[17]} E={fields[18]}')
        (args.out / f"{family}.txt").write_text("\n".join(listing) + "\n")
        rows.append(dict(family=family, registration=f"0x{entry:08x}",
                         source=str(source), line=line,
                         body_sha256=hashlib.sha256(generated.encode()).hexdigest(),
                         image=f"0x{code:08x}", size_address=f"0x{size_address:08x}",
                         instructions=count, sha256=hashlib.sha256(image).hexdigest()))
    (args.out / "programs.json").write_text(json.dumps(
        dict(elf_sha256=ELF_SHA, method=__doc__, programs=rows), indent=2) + "\n")
    for row in rows:
        print(f'{row["family"]}: {row["instructions"]} static instructions, {row["sha256"]}')


if __name__ == "__main__":
    main()
