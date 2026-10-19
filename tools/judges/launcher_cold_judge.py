#!/usr/bin/env python3
"""tools/judges/launcher_cold_judge.py -- the launcher's cold run, judged.

With HOME pointed at an EMPTY folder, the launcher (driven by the engine's Qt Test program test_launcher_cold, which
presses the window's own buttons) must: install everything it needs into app-data, find the disc image in a given
folder by its fingerprint, build the macOS target, press Play, and the game must reach its title state (Title (7)) in
the launcher's own play log -- with zero shell commands typed by a person. The wall time and the download size are
recorded.

RED when there is no launcher (no test_launcher_cold in --launcher-build, or it fails); GREEN when every step above
holds. Frozen: the thresholds are the manifest's (the title state name) and the time limits below.

  tools/judges/launcher_cold_judge.py --launcher-build DIR --iso IMAGE --out DIR [--manifest launcher/title.json]
                                      [--install-timeout-min 90] [--build-timeout-min 120] [--title-timeout-s 300]
                                      [--keep-home]

Writes <out>/report.json, <out>/cold.log and <out>/screenshots/*.png; exit 0 GREEN, 1 RED.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True, help="the engine's launcher build dir (holds test_launcher_cold)")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "launcher", "title.json"))
    ap.add_argument("--install-timeout-min", type=int, default=90)
    ap.add_argument("--build-timeout-min", type=int, default=120)
    ap.add_argument("--title-timeout-s", type=int, default=300)
    ap.add_argument("--keep-home", action="store_true", help="leave the temp HOME behind (for inspection)")
    ap.add_argument("--home", default="", help="use this (empty) folder as HOME instead of a fresh temp one")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_cold_judge", "result": "RED", "reasons": []}
    driver = os.path.join(a.launcher_build, "test_launcher_cold")
    if not os.path.isfile(driver) or not os.access(driver, os.X_OK):
        report["reasons"].append("no launcher: %s does not exist (there is no launcher to judge)" % driver)
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_cold_judge: RED -- " + report["reasons"][0])
        return 1
    if not os.path.isfile(a.iso):
        report["reasons"].append("no disc image at %s" % a.iso)
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_cold_judge: RED -- " + report["reasons"][0])
        return 1

    # the empty HOME (the cold machine) and the folder holding the disc (the user's "given folder")
    home = a.home or tempfile.mkdtemp(prefix="te-launcher-cold-home-")
    if os.listdir(home):
        print("launcher_cold_judge: HOME %s is not empty -- refusing (the run must be cold)" % home)
        return 2
    iso_dir = os.path.join(home, "disc")
    os.makedirs(iso_dir)
    target = os.path.join(iso_dir, os.path.basename(a.iso))
    try:
        os.link(a.iso, target)   # a hard link: no copy, and the scanner reads a real file (symlinks are skipped)
    except OSError:
        shutil.copy2(a.iso, target)

    # the manifest with the source pinned at THIS checkout and THIS commit (the launcher clones it; the engine and the
    # game code are trees in it, so nothing comes from the network)
    with open(a.manifest) as f:
        manifest = json.load(f)
    manifest["source"]["url"] = ROOT
    manifest["source"]["commit"] = git("rev-parse", "HEAD")
    titles = os.path.join(home, "titles")
    os.makedirs(titles)
    manifest_path = os.path.join(titles, "title.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    shots = os.path.join(a.out, "screenshots")
    os.makedirs(shots, exist_ok=True)
    report_path = os.path.join(a.out, "cold_report.json")
    env = dict(os.environ)
    env.update({
        "HOME": home,
        "PS2R_COLD_MANIFEST": manifest_path,
        "PS2R_COLD_ISO_DIR": iso_dir,
        "PS2R_COLD_REPORT": report_path,
        "PS2R_COLD_SCREENSHOTS": shots,
        "PS2R_COLD_INSTALL_TIMEOUT_MIN": str(a.install_timeout_min),
        "PS2R_COLD_BUILD_TIMEOUT_MIN": str(a.build_timeout_min),
        "PS2R_COLD_TITLE_TIMEOUT_S": str(a.title_timeout_s),
        # Qt Test's per-function limit (ms): the install and the build each live inside one test function
        "QTEST_FUNCTION_TIMEOUT": str(max(a.install_timeout_min, a.build_timeout_min) * 60 * 1000 + 60000),
    })
    # The launcher must not inherit the developer's toolchain pins or caches: a clean PATH (system + Homebrew's git
    # if that is where git is) and no VITASDK.
    env.pop("VITASDK", None)
    env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    t0 = time.time()
    print("launcher_cold_judge: HOME=%s  driver=%s" % (home, driver))
    with open(os.path.join(a.out, "cold.log"), "w") as log:
        proc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT,
                              timeout=(a.install_timeout_min + a.build_timeout_min) * 60 + a.title_timeout_s + 600)
    wall = time.time() - t0
    report["wall_seconds"] = round(wall, 1)
    report["driver_exit"] = proc.returncode
    cold = {}
    if os.path.isfile(report_path):
        with open(report_path) as f:
            cold = json.load(f)
    report["cold"] = cold
    report["home"] = home
    report["screenshots"] = sorted(os.listdir(shots))

    ok = proc.returncode == 0
    if not ok:
        report["reasons"].append("the driver failed (exit %d): see cold.log" % proc.returncode)
    steps = cold.get("install_steps", {})
    for sid, s in steps.items():
        if sid != "vitasdk" and s.get("state") != 2:
            report["reasons"].append("install step %s not done: %s" % (sid, s.get("reason") or s.get("detail")))
    if not cold.get("build_ok"):
        report["reasons"].append("the macOS build did not succeed: %s" % cold.get("build_failure", "no record"))
    if not cold.get("title_reached"):
        report["reasons"].append("the game did not reach %s in the launcher's play log" % manifest.get("run", {}).get("title_state_name", "Title"))
    report["download_bytes"] = cold.get("download_bytes")
    report["on_disk_bytes"] = cold.get("on_disk_bytes")
    report["install_seconds"] = cold.get("install_seconds")
    report["build_seconds"] = cold.get("build_seconds")
    report["title_seconds"] = cold.get("title_seconds")
    report["result"] = "GREEN" if ok and not report["reasons"] else "RED"
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
    print("launcher_cold_judge: %s -- wall %.0f s; install %s s, download %s bytes; build %s s; title after %s s; %d screenshots" % (
        report["result"], wall, cold.get("install_seconds"), cold.get("download_bytes"), cold.get("build_seconds"),
        cold.get("title_seconds"), len(report["screenshots"])))
    for r in report["reasons"]:
        print("  - " + r)
    if not a.keep_home and report["result"] == "GREEN" and not a.home:
        shutil.rmtree(home, ignore_errors=True)
    return 0 if report["result"] == "GREEN" else 1


if __name__ == "__main__":
    sys.exit(main())
