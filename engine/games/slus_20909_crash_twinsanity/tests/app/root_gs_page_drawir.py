#!/usr/bin/env python3
"""RootRoutesGsPageAndDrawIr -- the composition root routes the GS privileged
page to GsState and feeds the GIF stream to the GS façade (INTEGRATION_PLAN items
6.2 / 6.4).

Runs the built `nativeee` binary exactly as the harness does for an anchor leg
(verify/run_leg.py: `--profile deterministic --headless --harness-out <dir>
--frames N --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on the produced run.meta and leg artifacts:

  * unhandled_mmio_pages carries no 0x12000000 / 0x12001000 page  -- the
    GsMmioHandler range displaced the MmioSink's GS_CSR / GS_PRIV rows (before
    6.2 every anchor leg listed 0x12000000, coldboot both pages)
  * gs_priv_writes > 0 and gs_priv_mmio w > 0                      -- the
    title's DISPFB/DISPLAY stores reach the ONE GsPrivRegs shadow through the page
  * drawir=draws>0,jobs>0                                          -- the
    GsStateSink fed GsState and the counting DrawIrSink saw the flushed draws
    (the anchors render)
  * gs_sink regs/kicks/eops > 0                                    -- the
    fan-out delivered the stream to the façade
  * gsops_ops > 0                                                  -- the
    recorder is still attached (the second fan-out member)
  * classes= (video=gl) routes a stamped leg's draws into the title's stamps
    classes (the consumer resolved the handler id to the entry's name); a root
    that leaves the name unbound reports every stamped draw as hle-other
  * drawir.ohash exists with one `<boundary>,0x<16 hex>` line per frame,
    beside gsops.ohash with the same line count
  * finalized=1, frames=N

A real process run, not a self-report: a root that does not register the GS
range lists 0x12000000 in unhandled_mmio_pages; one that keeps the recorder as
the parser's only sink reports drawir=draws=0. Exit 77 (SKIP) when the title
files or the anchor are absent.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
OHASH_LINE = re.compile(r"^(\d+),0x([0-9a-f]{16})$")


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def parse_fields(value):
    """'a=1,b=2' -> {'a': '1', 'b': '2'}"""
    out = {}
    for part in value.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def ohash_lines(path):
    if not os.path.isfile(path):
        return None
    lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line:
                lines.append(line)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.anchor):
        print("SKIP: anchor absent: %s" % args.anchor)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_gs_page_drawir_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0
    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("unhandled_mmio", "unhandled_mmio_pages", "gs_priv_writes", "gs_priv_reads", "gs_priv_mmio",
                "gs_sink", "drawir", "classes", "video", "gsops_ops", "ticks", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    def as_int(text):
        try:
            return int(text)
        except (TypeError, ValueError):
            return -1

    check(proc.returncode == 0, "bounded run exits 0")
    pages = [p for p in meta.get("unhandled_mmio_pages", "").split(",") if p]
    gs_pages = [p for p in pages if p.lower().startswith("0x1200")]
    check("unhandled_mmio_pages" in meta and not gs_pages,
          "unhandled_mmio_pages has no GS privileged page (0x12000000 / 0x12001000)")
    check(as_int(meta.get("gs_priv_writes")) > 0, "gs_priv_writes > 0 (the title's stores reach the one shadow)")
    mmio = meta.get("gs_priv_mmio", "")
    m = re.match(r"^r(\d+),w(\d+)$", mmio)
    check(m is not None and int(m.group(2)) > 0, "gs_priv_mmio=r<n>,w<n> with w > 0 (stores arrived through the page)")
    sink = parse_fields(meta.get("gs_sink", ""))
    check(as_int(sink.get("regs")) > 0 and as_int(sink.get("kicks")) > 0 and as_int(sink.get("eops")) > 0,
          "gs_sink regs/kicks/eops > 0 (the fan-out fed the façade)")
    drawir = parse_fields(meta.get("drawir", ""))
    check(as_int(drawir.get("draws")) > 0, "drawir draws > 0 (the façade flushed draws into the counting sink)")
    check(as_int(drawir.get("jobs")) > 0, "drawir jobs > 0 (transfer / priv-reg jobs reached the sink)")
    check(re.match(r"^0x[0-9a-f]{1,16}$", drawir.get("hash", "")) is not None, "drawir hash is a hex word (run.meta Hex() is unpadded)")
    check(as_int(meta.get("gsops_ops")) > 0, "gsops_ops > 0 (the recorder stays attached)")
    # The identity stamp (invariant 28, integration 6.4-B): HLE'd PATH1 kicks
    # reach the façade stamped (the runtime's observer -> GsStateSink slot FIFO ->
    # SetIdentity at delivery); no PATH1 packet arrives without its slot.
    check(as_int(drawir.get("identity_stamped")) > 0, "drawir identity_stamped > 0 (HLE'd kicks stamped at delivery)")
    check(as_int(drawir.get("identity_underflow")) == 0, "drawir identity_underflow = 0 (every PATH1 packet had its slot)")
    # The consumer half of the stamp: the root resolves a stamped draw's handler
    # id back to the library entry's name, so the class registry routes HLE'd
    # draws into the title's `stamps` classes. Without the name every stamped
    # draw lands in hle-other (no renderer -> generic), so a stamped leg that
    # routes only hle-other among the HLE classes fails here. video=stub legs
    # carry no registry (no classes= key) -- the check is skipped honestly then.
    stamps_classes = ("world-tristrip", "skinned", "particles-untex", "indexed-displace", "sprites-HUD")
    if meta.get("video") == "gl":
        classes = parse_fields(meta.get("classes", ""))
        check("classes" in meta and all(k in classes for k in stamps_classes + ("hle-other",)),
              "classes= census names every class (video=gl)")
        routed = sum(max(as_int(classes.get(k)), 0) for k in stamps_classes)
        check(as_int(drawir.get("identity_stamped")) <= 0 or routed > 0,
              "classes: a stamped leg routes draws into the stamps classes (handler name resolved), not hle-other only"
              " (stamps=%d, hle-other=%s)" % (routed, classes.get("hle-other", "<absent>")))
    else:
        print("  SKIP classes= checks (video=%s: no class registry)" % meta.get("video", "<absent>"))
    ticks = parse_fields(meta.get("ticks", ""))
    check(as_int(ticks.get("boundary")) == args.frames,
          "ticks boundary=%d (one Boundary-cause tick per frame; per-cause census present)" % args.frames)
    check(all(k in ticks for k in ("boundary", "cdvd", "demand", "deferred")), "ticks= names every TickSync cause")
    draw_lines = ohash_lines(os.path.join(out_dir, "drawir.ohash"))
    gsops_lines = ohash_lines(os.path.join(out_dir, "gsops.ohash"))
    check(draw_lines is not None and len(draw_lines) == args.frames,
          "drawir.ohash has %d lines" % args.frames)
    check(draw_lines is not None and all(OHASH_LINE.match(l) for l in draw_lines) and
          [l.split(",")[0] for l in (draw_lines or [])] == [str(i) for i in range(args.frames)],
          "drawir.ohash lines are `<boundary>,0x<16 hex>` for boundaries 0..N-1")
    check(gsops_lines is not None and draw_lines is not None and len(gsops_lines) == len(draw_lines),
          "gsops.ohash and drawir.ohash have the same line count")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootRoutesGsPageAndDrawIr: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
