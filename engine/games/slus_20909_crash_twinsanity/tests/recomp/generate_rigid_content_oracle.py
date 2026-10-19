"""Compile unchanged NEW rigid/placed producers into an isolated test address space."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from recomp_paths import label, resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)

BODIES = [(0, entry) for entry in (
    0x18e570, 0x18e5a0, 0x1a1de8, 0x1a06a8, 0x19c638,
    0x1bd000, 0x1be7f0, 0x1be9f8, 0x1ba3a8, 0x1c0490,
    0x1bbec0, 0x1cc580, 0x1da060, 0x1d45e0, 0x1d49c8,
    0x19ef90, 0x1d8ae0, 0x1d8af8, 0x1d88d8, 0x1da2d8, 0x1c0410)]
# GetDiskManager's cold initialization is outside a loaded-asset fixture.
# These calls abort if reached; only the byte allocator is serviced.
EXTERNAL = {0x1815c0, 0x204530, 0x205298, 0x2c7678, 0x2c7718, 0x2030f8}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    declarations, bodies, audit = [], [], []
    directory = Path("games/slus_20909_crash_twinsanity/recomp")
    source_map = {}
    for path in sorted(resolve(args.root, directory).glob("recompiled_shard_*.cpp")):
        text = path.read_text()
        for match in re.finditer(r"^void FUN_([0-9a-f]{8})\(", text, re.M):
            entry = int(match[1], 16)
            source_map[entry] = (label(path, args.root), text, match.start())
    pending = {entry for _, entry in BODIES}
    visited = set()
    while pending:
        entry = min(pending)
        pending.remove(entry)
        visited.add(entry)
        path, text, start = source_map[entry]
        name = f"FUN_{entry:08x}"
        body = text[start:text.index("\n// Function:", start)]
        calls = {int(call, 16) for call in re.findall(r"recomp_call\([^,]+, (0x[0-9a-f]+)u", body)}
        pending |= calls - visited - EXTERNAL
        if len(visited | pending) > 120:
            raise ValueError("producer closure grew; review the newly reached subsystem")
        declarations.append(f"void {name}(CpuContext&, uint32_t);")
        bodies.append(body)
        audit.append(dict(entry=f"0x{entry:08x}", source=str(path),
                          line=text[:start].count("\n") + 1,
                          source_sha256=hashlib.sha256(body.encode()).hexdigest(),
                          omitted=[], calls=[f"0x{x:08x}" for x in sorted(calls)],
                          indirect_sites=body.count("recomp_indirect_call(")))
    dispatch = ["void Original(uint32_t address, CpuContext& context) { switch(address) {"]
    dispatch += [f"case 0x{entry:x}: FUN_{entry:08x}(context, address); return;" for entry in sorted(visited)]
    dispatch += ["default: External(address, context); return; }}"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "rigid_content_generated.cpp").write_text(
        '#include "games/slus_20909_crash_twinsanity/tests/recomp/rigid_content_oracle_memory.h"\n'
        '#pragma STDC FENV_ACCESS ON\nnamespace rigid_content_oracle {\n' +
        "\n".join(declarations + [
            "void recomp_call(uint32_t, uint32_t address, CpuContext& context) { Run(address, context); }"
        ] + bodies + dispatch) + "\n}\n")
    (args.out / "rigid_content_generated.json").write_text(json.dumps(dict(
        oracle="NEW complete generated bodies", bodies=audit,
        indirect_targets=["0x001d8ae0 (type-01 program query)", "0x001d49c8 (type-01 material)"],
        external_calls={"0x001815c0": "test byte allocator; returns disjoint writable storage",
                        "0x00204530": "test aligned byte allocator; honours requested alignment"},
        unreachable_external=[f"0x{x:08x}" for x in sorted(EXTERNAL - {0x1815c0, 0x204530})]),
        indent=2) + "\n")


if __name__ == "__main__":
    main()
