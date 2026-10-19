#!/usr/bin/env python3
"""check_module.py -- the linked module's contract: imports == env.memory + a SUBSET of the seam list (nothing else),
the retail image sits exactly at its retail address and is the module's data end (__data_end == __heap_base == image end:
the game's heap layout does not move with the build), the required exports exist. Fails the build otherwise.

    check_module.py --wasm-objdump WASM_OBJDUMP --wasm game.wasm --imports seam_imports.txt --image-start N --image-end N --report out.json
"""
import argparse
import json
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-objdump", required=True)
    ap.add_argument("--wasm", required=True)
    ap.add_argument("--imports", required=True)
    ap.add_argument("--image-start", type=int, required=True)
    ap.add_argument("--image-end", type=int, required=True)
    ap.add_argument("--report", required=True)
    a = ap.parse_args()
    allowed = {l.strip() for l in open(a.imports) if l.strip()}
    dump = subprocess.run([a.wasm_objdump, "-x", a.wasm], capture_output=True, text=True, check=True).stdout
    imports, bad, memory_import = [], [], None
    for m in re.finditer(r"- (func|memory|table|global)\[\d+\].*?<- (\S+)\.(\S+)", dump):
        kind, mod, name = m.groups()
        if kind == "memory":
            memory_import = f"{mod}.{name}"
            continue
        imports.append(f"{mod}.{name}")
        if mod != "env" or name not in allowed or kind != "func":
            bad.append(f"{kind} {mod}.{name}")
    exports = re.findall(r'-> "([^"]+)"', dump)
    funcs = re.search(r"Function\[(\d+)\]", dump)
    elem = re.search(r"segment\[0\] flags=\d+ table=0 count=(\d+)", dump)
    seg0 = re.search(r"- segment\[0\] <[^>]*> memory=0 size=(\d+) - init i32=(\d+)", dump)
    problems = []
    if memory_import != "env.memory":
        problems.append(f"memory must be imported as env.memory (got {memory_import})")
    if bad:
        problems.append("imports outside the seam: " + ", ".join(bad))
    for e in ("entry", "te_retail_image", "te_retail_functions", "te_retail_function_count", "__indirect_function_table"):
        if e not in exports:
            problems.append(f"missing export {e}")
    globs = {m.group(1): int(m.group(2)) for m in re.finditer(r"<([^>]+)> - init i32=(\d+)", dump)}
    if globs.get("te_retail_image") != a.image_start:
        problems.append(f"the retail image must sit at {a.image_start} (got {globs.get('te_retail_image')})")
    for name in ("__data_end", "__heap_base"):
        if globs.get(name) != a.image_end:
            problems.append(f"{name} must be the retail image's end {a.image_end} (got {globs.get(name)}): the C++'s data must sit below the image")
    if seg0 and int(seg0.group(2)) + int(seg0.group(1)) > a.image_start:
        problems.append(f"the first data segment ({seg0.group(2)}+{seg0.group(1)}) overlaps the retail image")
    rep = {"imports": sorted(imports), "import_count": len(imports), "seam_size": len(allowed), "exports": exports,
           "functions": int(funcs.group(1)) if funcs else None, "table_entries": int(elem.group(1)) if elem else None,
           "image_segment_start": globs.get("te_retail_image"), "cxx_data_start": int(seg0.group(2)) if seg0 else None,
           "data_end": globs.get("__data_end"), "problems": problems}
    json.dump(rep, open(a.report, "w"), indent=1)
    if problems:
        print("check_module: FAILED\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    print(f"check_module: OK -- {len(imports)} imports (all in the seam of {len(allowed)}), {rep['functions']} functions, "
          f"{rep['table_entries']} table entries, image at 0x{a.image_start:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
