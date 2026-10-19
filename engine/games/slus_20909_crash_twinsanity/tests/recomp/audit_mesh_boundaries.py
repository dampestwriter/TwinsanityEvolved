"""Static NTSC mesh boundary references; no play, dump, or emulator input."""
import argparse
import bisect
import hashlib
import json
import re
import struct
from pathlib import Path
from recomp_paths import label, resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

ENTRIES = {
    0x1bd000: "SetRigidModelDma_", 0x1be7f0: "SetPlacedModelDMA",
    0x1be9f8: "SetDefaultMeshDMA (guarded; unselected)",
    0x19c638: "CloseInstanceBlocks (shared software bookkeeping)",
    0x1ba3a8: "FlushMaterial", 0x1c0410: "ForgetMaterials (shared software bookkeeping)",
    0x1bcb58: "ModelRead (load notification)", 0x1c0b08: "ModelDestroy (load notification)",
}
ELF_SHA = "8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f"


def sections(elf):
    table = struct.unpack_from("<I", elf, 32)[0]
    stride, count, names = struct.unpack_from("<HHH", elf, 46)
    headers = [struct.unpack_from("<10I", elf, table + i * stride) for i in range(count)]
    h = headers[names]
    strings = elf[h[4]:h[4] + h[5]]
    for header in headers:
        at = header[0]
        name = strings[at:strings.index(b"\0", at)].decode()
        if header[1] != 8:
            yield name, header[3], elf[header[4]:header[4] + header[5]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    elf = args.elf.read_bytes()
    digest = hashlib.sha256(elf).hexdigest()
    if digest != ELF_SHA:
        raise ValueError("unreviewed retail ELF")
    sources, starts = {}, []
    for path in sorted(resolve(args.root, "games/slus_20909_crash_twinsanity/recomp").glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        for match in re.finditer(r"^void FUN_([0-9a-f]{8})\(", text, re.M):
            address = int(match[1], 16)
            starts.append(address)
            if address in ENTRIES:
                end = text.find("\n// Function:", match.start())
                body = text[match.start():end if end != -1 else len(text)]
                sources[address] = dict(name=ENTRIES[address], entry=f"0x{address:08x}",
                    source=label(path, args.root), line=text[:match.start()].count("\n") + 1,
                    body_sha256=hashlib.sha256(body.encode()).hexdigest(),
                    interior_entries=sorted(set(int(v, 16) for v in re.findall(r"case (0x[0-9a-f]+): goto", body))),
                    direct_calls=sorted(set(int(v, 16) for v in re.findall(r"recomp_call\([^,]+, (0x[0-9a-f]+)u", body))),
                    external_jumps=[], data_references=[])
    starts.sort()
    owner = lambda pc: starts[bisect.bisect_right(starts, pc) - 1]
    targets = {}
    for entry, row in sources.items():
        for target in [entry] + row["interior_entries"]:
            targets[target] = entry
    for name, base, data in sections(elf):
        if name == ".text":
            for offset in range(0, len(data) - 3, 4):
                word = struct.unpack_from("<I", data, offset)[0]
                if word >> 26 not in (2, 3):
                    continue
                pc = base + offset
                target = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
                if target in targets and owner(pc) != targets[target]:
                    sources[targets[target]]["external_jumps"].append(dict(
                        instruction=f"0x{pc:08x}", caller=f"0x{owner(pc):08x}",
                        target=f"0x{target:08x}", kind="JAL" if word >> 26 == 3 else "J"))
        elif name in (".rodata", ".data", ".sdata", ".got"):
            for offset in range(0, len(data) - 3, 4):
                target = struct.unpack_from("<I", data, offset)[0]
                if target in targets:
                    sources[targets[target]]["data_references"].append(dict(
                        section=name, address=f"0x{base+offset:08x}", target=f"0x{target:08x}"))
    for row in sources.values():
        row["interior_entries"] = [f"0x{value:08x}" for value in row["interior_entries"]]
        row["direct_calls"] = [f"0x{value:08x}" for value in row["direct_calls"]]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(elf_sha256=digest,
        method="J/JAL in ELF .text, aligned words in .rodata/.data/.sdata/.got, NEW generated body boundaries",
        limits="Static references are not a proof against computed addresses. Interior callbacks retain original continuations except during an already-selected pass, when an unexpected entry faults.",
        entries=[sources[entry] for entry in sorted(sources)]), indent=2) + "\n")
    for entry, row in sorted(sources.items()):
        print(f"{entry:08x} {row['name']}: {len(row['external_jumps'])} external jumps; {len(row['data_references'])} data references")


if __name__ == "__main__":
    main()
