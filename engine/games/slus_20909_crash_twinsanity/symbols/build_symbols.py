#!/usr/bin/env python3
"""build_symbols.py -- the title's function symbol table `symbols.tsv`
(adjudicated names <-> v2 addresses).

  build_symbols.py --write        regenerate symbols.tsv from the sources below (deterministic)
  build_symbols.py --check        the ctest: symbols.tsv is byte-identical to a fresh build, every
                                  address unique, every FUN_ address inside the ELF text the generated
                                  output covers, every adjudicated name resolvable to its source
  build_symbols.py --selftest     unit checks over synthetic inputs

Columns (tab-separated, `#` comments):
  address           the EE guest address (0x%08x)
  v2_name           the frozen recompiler's name: `FUN_<addr>` for a generated function start (the
                    8807 declared in recomp/recompiled.h + `entry`), or `FUN_<start>+0x<off>` for a
                    manifest row whose address is an inner label of a generated function
  adjudicated_name  the name a record of this tree gives the address (`-` when none)
  source            where the name comes from, one of:
                      sdk_manifest     games/<t>/hle/sdk_bindings.tsv (library function at a finder-matched address)
                      curated          symbols/adjudicated_names.tsv (hand-curated from the tree's records)
                      recomp_seam_label the generated TU's `// HLE Hook for <label>` comment (OLD's hook label;
                                        taken only where nothing better names the address -- three labels are
                                        known misnomers and are superseded by the manifest)
                      -                no adjudicated name
  status            `recompiled` (the generated body is the implementation) | `sdk_bound` (a manifest row the
                    root installs over the body) | `sdk_inner_label` (a manifest row at an inner label; never
                    installed) | `manual_body:<verdict>` (a verify/manual_body_ledger.tsv row: OLD installed a hand
                    body here; NEW runs the generated one) | `recomp_seam` (the generated TU replaces the body with a
                    `hle_<name>` call -- a seam of the frozen generator, resolved by the title glue or the engine)
Precedence for adjudicated_name: sdk_manifest > curated > recomp_seam_label.
"""
import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TITLE_DIR = os.path.dirname(HERE)
# The generated recomp TUs live in the BUILD directory (PS2R_RECOMP_DIR, cmake/TitleInputs.cmake);
# without the variable the historical in-tree location (a developer's regenerated copy) is used.
RECOMP = os.environ.get("PS2R_RECOMP_DIR") or os.path.join(TITLE_DIR, "recomp")
MANIFEST = os.path.join(TITLE_DIR, "hle", "sdk_bindings.tsv")
LEDGER = os.path.join(TITLE_DIR, "verify", "manual_body_ledger.tsv")
CURATED = os.path.join(HERE, "adjudicated_names.tsv")
OUT = os.path.join(HERE, "symbols.tsv")

DECL_RE = re.compile(r"^void (FUN_([0-9a-f]{8})|entry)\(CpuContext&", re.M)
INSTALL_RE = re.compile(r"recompiled_functions\[0x([0-9a-fA-F]+)\]\s*=")
RANGE_RE = re.compile(r"function_ranges\.push_back\(\{0x([0-9a-fA-F]+), 0x([0-9a-fA-F]+)")
LABEL_RE = re.compile(r"// HLE Hook for (.+)$")
# the generated TU's hook labels at these addresses are wrong (the finder's sceCdGetError / sceCdBreak /
# sceCdMmode); the manifest names them.
BAD_LABELS = {0x2c4578, 0x2c4610, 0x2c46c8}
# OLD's generic hook label on four unrelated addresses (0x2aac80, 0x2ad8f0, 0x2adb18, 0x2d6880): not a name.
BAD_LABEL_TEXT = ("Intitialize Graphics",)


def read_tsv(path):
    rows = []
    with open(path) as f:
        for line in f:
            s = line.rstrip("\n")
            if not s or s.startswith("#"):
                continue
            rows.append(s.split("\t"))
    return rows


def generated_functions(recomp_dir):
    """{addr: name} of the declared generated functions, the install table's starts and the spans."""
    decls = {}
    with open(os.path.join(recomp_dir, "recompiled.h")) as f:
        for m in DECL_RE.finditer(f.read()):
            name = m.group(1)
            decls[int(m.group(2), 16) if name != "entry" else 0x100008] = name
    starts, spans, labels = set(), [], {}
    pending = None
    with open(os.path.join(recomp_dir, "recompiled.cpp")) as f:
        for line in f:
            lm = LABEL_RE.search(line)
            if lm:
                pending = lm.group(1).strip()
                continue
            im = INSTALL_RE.search(line)
            if im:
                a = int(im.group(1), 16)
                starts.add(a)
                if pending is not None:
                    labels[a] = pending
                    pending = None
            rm = RANGE_RE.search(line)
            if rm:
                spans.append((int(rm.group(1), 16), int(rm.group(2), 16)))
    spans.sort()
    return decls, starts, spans, labels


def containing_span(spans, a):
    import bisect
    i = bisect.bisect_right(spans, (a, 0xffffffff)) - 1
    if i >= 0 and spans[i][0] <= a < spans[i][1]:
        return spans[i]
    return None


def sanitize_label(label):
    label = re.sub(r"\(.*?\)", "", label).strip()
    label = re.sub(r"[^A-Za-z0-9_]+", "_", label).strip("_")
    return label or "-"


def build(recomp_dir=RECOMP, manifest=MANIFEST, ledger=LEDGER, curated=CURATED):
    decls, starts, spans, labels = generated_functions(recomp_dir)
    man = {}
    for lib, fn, addr, *_ in read_tsv(manifest):
        man[int(addr, 16)] = (lib, fn)
    cur = {}
    for row in read_tsv(curated):
        if row[0] == "address":
            continue
        cur[int(row[0], 16)] = row[1]
    led = {}
    for row in read_tsv(ledger):
        if row[0] == "address":
            continue
        led[int(row[0], 16)] = row[4] if len(row) > 4 else "open"
    addrs = set(decls) | set(man) | set(cur) | set(led)
    out, stats = [], {"functions": 0, "inner": 0, "adjudicated": 0, "by_source": {}, "manual_body": 0, "sdk_bound": 0,
                      "unplaced": []}
    for a in sorted(addrs):
        if a in decls:
            v2 = decls[a]
        else:
            sp = containing_span(spans, a)
            if sp is None or sp[0] not in decls:
                stats["unplaced"].append(a)
                continue
            v2 = "%s+0x%x" % (decls[sp[0]], a - sp[0])
        if a in man:
            name, src = man[a][1], "sdk_manifest"
        elif a in cur:
            name, src = cur[a], "curated"
        elif a in labels and a not in BAD_LABELS and labels[a] not in BAD_LABEL_TEXT:
            name, src = sanitize_label(labels[a]), "recomp_seam_label"
        else:
            name, src = "-", "-"
        if a in man and a not in decls:
            status = "sdk_inner_label"
            stats["inner"] += 1
        elif a in man:
            status = "sdk_bound"
            stats["sdk_bound"] += 1
        elif a in led:
            status = "manual_body:%s" % led[a]
            stats["manual_body"] += 1
        elif a in labels:
            status = "recomp_seam"
            stats["seams"] = stats.get("seams", 0) + 1
        else:
            status = "recompiled"
        if a in decls:
            stats["functions"] += 1
        if name != "-":
            stats["adjudicated"] += 1
            stats["by_source"][src] = stats["by_source"].get(src, 0) + 1
        out.append((a, v2, name, src, status))
    text = ("# symbols.tsv -- SLUS_209.09 function symbols: every generated function start (recomp/recompiled.h) plus the\n"
            "# sce manifest's inner-label rows, with the adjudicated name where a record names the address. GENERATED by\n"
            "# games/<t>/symbols/build_symbols.py --write (ctest TitleSymbolsTable checks it is current); do not hand-edit.\n"
            "# Sources: sdk_manifest = hle/sdk_bindings.tsv; curated = symbols/adjudicated_names.tsv; recomp_seam_label =\n"
            "# the generated TU's `// HLE Hook for` labels. Status: recompiled | sdk_bound | sdk_inner_label | manual_body:<verdict> | recomp_seam.\n"
            "address\tv2_name\tadjudicated_name\tsource\tstatus\n")
    text += "".join("0x%08x\t%s\t%s\t%s\t%s\n" % r for r in out)
    return text, out, stats, (decls, starts, spans)


def check(text, out, stats, gen):
    decls, starts, spans = gen
    fails = []
    if not os.path.isfile(OUT):
        fails.append("symbols.tsv missing (run --write)")
    elif open(OUT).read() != text:
        fails.append("symbols.tsv is stale: differs from a fresh build (run --write)")
    addrs = [r[0] for r in out]
    if len(addrs) != len(set(addrs)):
        fails.append("duplicate addresses")
    lo, hi = min(s for s, _ in spans), max(e for _, e in spans)
    for a, v2, name, src, status in out:
        if not (lo <= a < hi):
            fails.append("0x%08x outside the generated text 0x%08x-0x%08x" % (a, lo, hi))
        if v2.startswith("FUN_") and "+" not in v2 and a not in decls:
            fails.append("0x%08x: %s is not a declared generated function" % (a, v2))
        if (name == "-") != (src == "-"):
            fails.append("0x%08x: name/source disagree (%s, %s)" % (a, name, src))
    if stats["unplaced"]:
        fails.append("addresses inside no generated function: %s" % ["0x%08x" % a for a in stats["unplaced"]])
    if stats["functions"] != len(decls):
        fails.append("function rows %d != declared %d" % (stats["functions"], len(decls)))
    return fails


def report(stats, gen):
    decls, starts, spans = gen
    print("symbols: %d generated functions (declared %d, install-table starts %d), %d inner-label manifest rows, "
          "%d adjudicated names (%s), %d sdk_bound, %d manual_body rows, %d recomp_seam rows"
          % (stats["functions"], len(decls), len(starts), stats["inner"], stats["adjudicated"],
             ", ".join("%s %d" % kv for kv in sorted(stats["by_source"].items())), stats["sdk_bound"], stats["manual_body"],
             stats.get("seams", 0)))


def selftest():
    ok = True

    def c(cond, what):
        nonlocal ok
        print("  %s %s" % ("PASS" if cond else "FAIL", what))
        ok &= bool(cond)

    tmp = tempfile.mkdtemp(prefix="build_symbols_")
    rd = os.path.join(tmp, "recomp")
    os.mkdir(rd)
    open(os.path.join(rd, "recompiled.h"), "w").write(
        "void entry(CpuContext& ctx, uint32_t addr);\nvoid FUN_00100100(CpuContext& ctx, uint32_t addr);\n"
        "void FUN_002c1d78(CpuContext& ctx, uint32_t addr);\nvoid FUN_002c4578(CpuContext& ctx, uint32_t addr);\n")
    open(os.path.join(rd, "recompiled.cpp"), "w").write(
        "    recompiled_functions[0x100008] = [](CpuContext& ctx, uint32_t addr) { entry(ctx, addr); };\n"
        "    function_ranges.push_back({0x100008, 0x100100, recompiled_functions[0x100008]});\n"
        "    recompiled_functions[0x100100] = [](CpuContext& ctx, uint32_t addr) { FUN_00100100(ctx, addr); };\n"
        "    function_ranges.push_back({0x100100, 0x100200, recompiled_functions[0x100100]});\n"
        "    // HLE Hook for sceGsSyncV\n"
        "    recompiled_functions[0x2c1d78] = [](CpuContext& ctx, uint32_t addr) {\n"
        "    function_ranges.push_back({0x2c1d78, 0x2c1e0c, recompiled_functions[0x2c1d78]});\n"
        "    // HLE Hook for sceCdStStat\n"
        "    recompiled_functions[0x2c4578] = [](CpuContext& ctx, uint32_t addr) {\n"
        "    function_ranges.push_back({0x2c4578, 0x2c4610, recompiled_functions[0x2c4578]});\n")
    man = os.path.join(tmp, "m.tsv")
    open(man, "w").write("# h\nlibcdvd.a\tsceCdGetError\t0x002c4578\t\nlibkernl.a\tInner\t0x002c4580\t\n")
    led = os.path.join(tmp, "l.tsv")
    open(led, "w").write("address\told_file\trow_class\tfirst_leg\tverdict\n0x00100100\tx\ty\t-\topen\n")
    cur = os.path.join(tmp, "c.tsv")
    open(cur, "w").write("address\tname\tsource\n0x00100008\tentry\tt\n0x002c4578\tWRONG\tt\n")
    text, out, stats, gen = build(rd, man, led, cur)
    rows = {r[0]: r for r in out}
    c(len(out) == 5 and stats["functions"] == 4 and stats["inner"] == 1, "4 functions + 1 inner label row")
    c(rows[0x2c4578][2:] == ("sceCdGetError", "sdk_manifest", "sdk_bound"), "the manifest name wins over curated and over the bad seam label")
    c(rows[0x2c4580][1:] == ("FUN_002c4578+0x8", "Inner", "sdk_manifest", "sdk_inner_label"), "an inner label is FUN_<start>+off, status sdk_inner_label")
    c(rows[0x100100][2:] == ("-", "-", "manual_body:open"), "a ledger row with no name: status manual_body:<verdict>, name -")
    c(rows[0x2c1d78][2:] == ("sceGsSyncV", "recomp_seam_label", "recomp_seam"), "a seam label names an address no table covers; status recomp_seam")
    c(rows[0x100008][2:] == ("entry", "curated", "recompiled"), "the curated name at the entry")
    fails = check(text, out, stats, gen)
    c(any("symbols.tsv" in f for f in fails) and not any("duplicate" in f or "outside" in f for f in fails),
      "check() over the synthetic build only complains about the real symbols.tsv not matching it")
    print("build_symbols selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    text, out, stats, gen = build()
    report(stats, gen)
    if a.write:
        open(OUT, "w").write(text)
        print("wrote %s (%d rows)" % (OUT, len(out)))
    if a.check or not a.write:
        fails = check(text, out, stats, gen)
        for f in fails:
            print("  " + f)
        print("TitleSymbolsTable: %s" % ("FAIL" if fails else "PASS"))
        return 1 if fails else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
