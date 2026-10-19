#!/usr/bin/env python3
"""Freeze unchanged NEW player buffer bodies for the Movie loan oracle."""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = Path("games/slus_20909_crash_twinsanity/recomp/recompiled_shard_029.cpp")
    text = resolve(args.root, source).read_text()
    entries = (0x2AE1C8, 0x2AE860, 0x2AF5D0, 0x2B1630)
    bodies, records = [], []
    for entry in entries:
        start = text.index(f"void FUN_{entry:08x}(")
        body = text[start:text.index("\n// Function:", start)]
        bodies.append(body)
        records.append({"entry": hex(entry), "source": str(source),
                        "source_sha256": hashlib.sha256(body.encode()).hexdigest(),
                        "compiled_sha256": hashlib.sha256(body.encode()).hexdigest()})
    prefix = """#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
namespace movie_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& c) { Dispatch(address, c); }
"""
    dispatch = ["bool RunGenerated(uint32_t address, CpuContext& c) { switch (address) {"]
    for entry in entries:
        dispatch.append(f"case {hex(entry)}: ObserveGenerated(address); FUN_{entry:08x}(c, address); return true;")
    dispatch += ["default: return false;", "} } }"]
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "movie_buffers_generated.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "movie_buffers_generated.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
