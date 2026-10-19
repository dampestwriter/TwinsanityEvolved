#!/usr/bin/env python3
"""link_wasm.py -- wasm-ld in two passes so the C++'s data sits BELOW the retail image and the module's data end is the
retail image's end, whatever the build (the game's replay depends on its heap layout; the heap must not move with
the C++'s data).

    link_wasm.py --wasm-ld WASM_LD --wasm-objdump WASM_OBJDUMP --image-start N --image-end N --stack-size N
                 --out game.wasm [--probe-base N] -- <wasm-ld inputs and flags>

Layout (wasm address == PS2 guest address):
    0 .. stack-size                 the shadow stack (--stack-first)
    global-base .. image-start      the C++'s .rodata/.data/.bss (global-base = image-start - their size)
    image-start .. image-end        the retail image: a .bss segment (zero; the runner fills it), the LAST data
    __data_end == __heap_base == image-end

Pass 1 links at a probe base (the stack's end) to measure the C++'s data; pass 2 links at image-start - size and checks
the image landed on its address and the data end is the image's end. Both passes get the same inputs and flags.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rspfile  # noqa: E402


def globals_of(objdump, wasm):
    dump = subprocess.run([objdump, "-x", "-j", "Global", wasm], capture_output=True, text=True, check=True).stdout
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"<([^>]+)> - init i32=(\d+)", dump)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-ld", required=True)
    ap.add_argument("--wasm-objdump", required=True)
    ap.add_argument("--image-start", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--image-end", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--stack-size", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--probe-base", type=lambda s: int(s, 0), default=None)
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args(rspfile.expand_argv(sys.argv[1:]))
    rest = a.rest[1:] if a.rest and a.rest[0] == "--" else a.rest
    probe = a.probe_base if a.probe_base is not None else a.stack_size

    def link(base, out):
        args = [*rest, "-o", out, f"--global-base={base}", "--stack-first", "-z", f"stack-size={a.stack_size}"]
        subprocess.run([a.wasm_ld, *rspfile.tool_args(args, out + ".rsp")], check=True)

    probe_out = a.out + ".pass1"
    link(probe, probe_out)
    g1 = globals_of(a.wasm_objdump, probe_out)
    if "te_retail_image" not in g1:
        print("link_wasm: pass 1 has no te_retail_image", file=sys.stderr)
        return 1
    size = g1["te_retail_image"] - probe
    if size < 0 or size % 16:
        print(f"link_wasm: pass 1 measured an odd C++ data size {size}", file=sys.stderr)
        return 1
    base = a.image_start - size
    if base < a.stack_size:
        print(f"link_wasm: the C++'s data ({size} bytes) does not fit below the retail image above the stack", file=sys.stderr)
        return 1
    link(base, a.out)
    g2 = globals_of(a.wasm_objdump, a.out)
    problems = []
    if g2.get("te_retail_image") != a.image_start:
        problems.append(f"retail image at 0x{g2.get('te_retail_image', 0):X}, wanted 0x{a.image_start:X}")
    for name in ("__data_end", "__heap_base"):
        if g2.get(name) != a.image_end:
            problems.append(f"{name} 0x{g2.get(name, 0):X}, wanted the image's end 0x{a.image_end:X}")
    if problems:
        print("link_wasm: FAILED\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    print(f"link_wasm: C++ data {size} bytes at 0x{base:X}..0x{a.image_start:X}, retail image 0x{a.image_start:X}..0x{a.image_end:X} "
          f"== __heap_base (room left below the image: {base - a.stack_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
