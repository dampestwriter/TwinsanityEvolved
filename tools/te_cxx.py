#!/usr/bin/env python3
"""te_cxx.py -- compile ONE game translation unit to a wasm32 object with ZERO edits to game code.

The decomp is written for GCC 15 / MIPS R5900 (n32). Five constructs in it are MIPS-only or GCC-15-only and the
feasibility report ("game-side inline asm") enumerates them: the two
`asm("" : "+f"(x))` float fences, the file-scope EABI thunks `asm((Abi::Thunk<...>(...)))` (a C++26/GCC-15 asm form
clang 18 does not parse; the thunks move registers between EABI and n32, which do not differ on wasm), the retained
hand-written MIPS thunk blocks `asm(R"(...)")` (same purpose, with stack arguments), one `lb` cache prefetch and one
`lq/sq` quadword copy. This wrapper preprocesses the TU with the pinned wasi-sdk clang, rewrites exactly those
constructs in the PREPROCESSED text (the repository is never touched), and compiles the result. Everything else --
asm labels (`RETAIL(name)`), the 1,352 layout pins, the GCC 2.9x vtables -- passes through unchanged.

Every rewrite is recorded; every thunk removed yields an ALIAS (retail name -> `<name>_n32`, the C++ function) that
the runner's retail-address table consumes (tools/gen_retail_table.py). Any `asm` statement this script does not
recognise is left in place so the compiler reports it: that is a blocker, listed per file in tools/compile_report.md.

    te_cxx.py --wasi-sdk DIR --game DIR --shim DIR [--std gnu++26] [-O2] [--report out.json] -o out.o file.cpp
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

CXXFLAGS_BASE = [
    "-std=gnu++26", "-D_EE", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "-fno-asynchronous-unwind-tables", "-fno-common", "-fno-strict-aliasing", "-ffunction-sections",
    "-fdata-sections", "-ffp-contract=off", "-fno-delete-null-pointer-checks", "-Wall", "-Wno-invalid-offsetof",
    # the decomp's RETAIL() asm labels on *declarations* are fine; clang warns about nothing here
]

# R1: the float fence. "+f" is a MIPS FPU constraint; "+m" pins the value through memory with the same intent.
RE_FENCE = re.compile(r'asm\s*\(\s*""\s*:\s*"\+f"\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\)\s*;')
# R2: file-scope EABI thunk made by Abi::Thunk (consteval asm text). Direction and name captured.
RE_THUNK = re.compile(r'asm\s*\(\s*\(\s*Abi::Thunk<.*?>\s*\(\s*Abi::Direction::(Export|Import)\s*,\s*"(\w+)"\s*\)\s*\)\s*\)\s*;',
                      re.DOTALL)
# R3: file-scope raw MIPS blocks.
RE_RAW = re.compile(r'asm\s*(?:volatile\s*)?\(\s*R"\((.*?)\)"\s*\)\s*;', re.DOTALL)
RE_RAW_GLOBL = re.compile(r'^\s*\.globl\s+(\w+)', re.MULTILINE)
RE_RAW_JUMP = re.compile(r'^\s*(?:jal|j)\s+(\w+)', re.MULTILINE)
# R4: the `lb $0` cache touch (scenery.cpp Touch).
RE_TOUCH = re.compile(r'asm\s+volatile\s*\(\s*"lb \$0, 0\(%0\)"\s*:\s*:\s*"r"\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\)\s*;')
# R5: the lq/sq quadword copy (eventcallers.cpp CopyQuadword): 16 bytes at the 16-byte-aligned addresses.
RE_LQSQ = re.compile(r'asm\s+volatile\s*\(\s*"lq \$8, 0\(%1\)\\n\\t"\s*"sq \$8, 0\(%0\)"\s*:\s*:\s*"r"\s*\(\s*(\w+)\s*\)\s*,'
                     r'\s*"r"\s*\(\s*(\w+)\s*\)\s*:\s*"\$8"\s*,\s*"memory"\s*\)\s*;')
# R6: abi.h declares CallEabiInteger (u64) and CallEabiFloat (f32) under ONE asm label "CallEabiTrampoline"; a wasm
# import has exactly one signature, so the float one gets its own import name (the runner implements both).
RE_TRAMP_F = re.compile(r'(f32\s+CallEabiFloat\s*\([^;]*?\)\s*asm\s*\(\s*"CallEabiTrampoline)(")')
# Anything else that still says asm as a STATEMENT (not an asm label after a declarator) -- reported, not rewritten.
RE_ANY_ASM_STMT = re.compile(r'(?<![\w.])asm\s*(?:volatile\s*)?\(\s*(?:R"\(|(?:"(?:[^"\\]|\\.)*"\s*)+:)')


def preprocess(clang, flags, src):
    cmd = [clang, "-E", *flags, str(src)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    return r.stdout, r.stderr


def rewrite(text):
    log = {"fences": 0, "thunks_export": [], "thunks_import": [], "raw_blocks": [], "touch": 0, "lqsq": 0,
           "unrecognised_asm": 0, "trampoline_f_renamed": 0}

    def fence(m):
        log["fences"] += 1
        return '__asm__("" : "+m"(%s));' % m.group(1)  # __asm__ spelling: the blocker scan below matches only `asm`
    text = RE_FENCE.sub(fence, text)

    def thunk(m):
        direction, name = m.group(1), m.group(2)
        if direction == "Export":
            log["thunks_export"].append(name)  # retail NAME -> C++ NAME_n32
            # EABI == n32 on wasm: the retail name IS the C++ function. An alias keeps every `asm("NAME")` reference and
            # every retail vtable/callback pointer (patched by name at run time) landing on the real function.
            return 'extern "C" void te_alias_%s(void) asm("%s") __attribute__((alias("%s_n32")));' % (name, name, name)
        log["thunks_import"].append(name)  # C++ NAME_n32 -> retail asm NAME (none exist in the tree)
        return ";"
    text = RE_THUNK.sub(thunk, text)

    def raw(m):
        body = m.group(1)
        globls = RE_RAW_GLOBL.findall(body)
        jumps = [j for j in RE_RAW_JUMP.findall(body)]
        entry = {"defines": globls, "jumps": jumps}
        # The retained blocks are all thunks `NAME:` ... `jal NAME_n32`: alias NAME -> NAME_n32. Anything else
        # (CallEabiTrampoline: `jalr $25`) is a RUNTIME-PROVIDED symbol the platform layer must define.
        aliases = [(g, g + "_n32") for g in globls if (g + "_n32") in jumps]
        entry["aliases"] = aliases
        entry["runtime_provided"] = [g for g in globls if (g + "_n32") not in jumps]
        log["raw_blocks"].append(entry)
        # The hand-written thunks (stack arguments) alias the same way; CallEabiTrampoline (no _n32 target) is an import.
        return " ".join('extern "C" void te_alias_%s(void) asm("%s") __attribute__((alias("%s")));' % (a[0], a[0], a[1])
                        for a in aliases) or ";"
    text = RE_RAW.sub(raw, text)

    def touch(m):
        log["touch"] += 1
        return "(void)(%s);" % m.group(1)
    text = RE_TOUCH.sub(touch, text)

    def lqsq(m):
        log["lqsq"] += 1
        to, frm = m.group(1), m.group(2)
        return ("__builtin_memcpy((void*)((unsigned int)(%s) & ~15u), (const void*)((unsigned int)(%s) & ~15u), 16);"
                % (to, frm))
    text = RE_LQSQ.sub(lqsq, text)

    text, n6 = RE_TRAMP_F.subn(r"\1F\2", text)
    log["trampoline_f_renamed"] = n6
    # Portable asm the wasm backend accepts as written: an EMPTY template with only memory ("=m"/"+m") operands
    # (particletrails.cpp's `asm volatile("" : "=m"(leftover))`). Counted, kept, not a blocker.
    RE_PORTABLE = re.compile(r'(?<![\w.])asm\s*(?:volatile\s*)?\(\s*""\s*:\s*"[=+]m"\s*\([^)]*\)\s*\)\s*;')
    log["kept_portable_asm"] = len(RE_PORTABLE.findall(text))
    log["unrecognised_asm"] = len(RE_ANY_ASM_STMT.findall(text)) - log["kept_portable_asm"]
    return text, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasi-sdk", required=True)
    ap.add_argument("--game", required=True, help="the game/ tree's root (has include/)")
    ap.add_argument("--shim", required=True, help="platform/nativeee/sdk_shim")
    ap.add_argument("--std", default="gnu++26")
    ap.add_argument("-O", dest="opt", default="2")
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("-D", dest="defs", action="append", default=[])
    ap.add_argument("--report", default=None)
    ap.add_argument("--keep-ii", default=None, help="write the rewritten preprocessed text here")
    ap.add_argument("-o", dest="out", required=True)
    ap.add_argument("src")
    a = ap.parse_args()

    sdk = Path(a.wasi_sdk)
    clang = str(sdk / "bin" / "clang++")
    sysroot = str(sdk / "share" / "wasi-sysroot")
    target = ["--target=wasm32-wasi", f"--sysroot={sysroot}"]
    flags = [*target, *CXXFLAGS_BASE, f"-O{a.opt}"]
    flags = [f if not f.startswith("-std=") else f"-std={a.std}" for f in flags]
    inc = [f"-I{Path(a.game) / 'include'}", f"-I{a.shim}", *[f"-I{i}" for i in a.incs]]
    defs = [f"-D{d}" for d in a.defs]

    report = {"src": a.src, "out": a.out, "ok": False, "stage": None, "rewrites": None, "errors": [], "stderr": ""}
    pp, err = preprocess(clang, [*flags, *inc, *defs], a.src)
    if pp is None:
        report["stage"] = "preprocess"
        report["stderr"] = err
        report["errors"] = [l for l in err.splitlines() if "error:" in l]
    else:
        text, log = rewrite(pp)
        report["rewrites"] = log
        if a.keep_ii:
            Path(a.keep_ii).write_text(text)
        cmd = [clang, *flags, "-x", "c++", "-c", "-o", a.out, "-"]
        r = subprocess.run(cmd, input=text, capture_output=True, text=True)
        report["stderr"] = r.stderr
        report["errors"] = [l for l in r.stderr.splitlines() if "error:" in l]
        report["ok"] = r.returncode == 0
        report["stage"] = "compile"
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
    if a.report:
        Path(a.report).parent.mkdir(parents=True, exist_ok=True)
        Path(a.report).write_text(json.dumps(report, indent=1))
    if not report["ok"] and report["stage"] == "preprocess":
        sys.stderr.write(report["stderr"])
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
