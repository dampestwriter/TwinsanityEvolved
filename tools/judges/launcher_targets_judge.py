#!/usr/bin/env python3
"""tools/judges/launcher_targets_judge.py -- the launcher's Targets page, judged.

In a HOME where the launcher's first run is complete (the cold judge's HOME, kept), the engine's Qt Test driver
test_launcher_targets checks the selector (exactly the manifest's targets; this machine's preselected; a target this
host cannot build greyed WITH its reason) and presses Build for the requested target:
  * PS Vita: a .vpk must come out (VitaSDK installed into app-data first), and its eboot must boot in Vita3K to the
    game's title state through tools/vita/vita3k_run.sh (the app log carries the game state line);
  * Windows: on a Windows host the release_dist .zip is produced, unpacked and listed; on this Mac the card must be
    greyed with the reason -- recorded as PENDING a Windows host, not RED.
RED when there is no launcher, the selector is wrong, a greyed target lacks its reason, or the requested artefact is
missing / does not boot.

  tools/judges/launcher_targets_judge.py --launcher-build DIR --home DIR --out DIR [--target vita|windows]
                                         [--manifest PATH] [--vita3k-seconds 180] [--skip-vita3k]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True)
    ap.add_argument("--home", required=True, help="a HOME where the first run is complete")
    ap.add_argument("--out", required=True)
    ap.add_argument("--target", default="vita")
    ap.add_argument("--manifest", default="")
    ap.add_argument("--vita3k-seconds", type=int, default=180)
    ap.add_argument("--skip-vita3k", action="store_true")
    ap.add_argument("--timeout-min", type=int, default=180)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_targets_judge", "target": a.target, "result": "RED", "reasons": []}
    driver = os.path.join(a.launcher_build, "test_launcher_targets")
    if not os.access(driver, os.X_OK):
        report["reasons"].append("no launcher: %s does not exist" % driver)
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_targets_judge: RED -- " + report["reasons"][0])
        return 1
    manifest_path = a.manifest or os.path.join(a.home, "titles", "title.json")
    if not os.path.isfile(manifest_path):
        report["reasons"].append("no manifest at %s" % manifest_path)
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_targets_judge: RED -- " + report["reasons"][0])
        return 1
    with open(manifest_path) as f:
        manifest = json.load(f)
    shots = os.path.join(a.out, "screenshots")
    report_path = os.path.join(a.out, "targets_report.json")
    env = dict(os.environ, HOME=a.home, PS2R_TARGETS_MANIFEST=manifest_path, PS2R_TARGETS_REPORT=report_path,
               PS2R_TARGETS_SCREENSHOTS=shots, PS2R_TARGETS_TARGET=a.target, PS2R_TARGETS_TIMEOUT_MIN=str(a.timeout_min),
               QTEST_FUNCTION_TIMEOUT=str(a.timeout_min * 60 * 1000 + 60000))
    env.pop("VITASDK", None)
    env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    with open(os.path.join(a.out, "targets.log"), "w") as log:
        proc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=a.timeout_min * 60 + 600)
    report["driver_exit"] = proc.returncode
    drv = {}
    if os.path.isfile(report_path):
        with open(report_path) as f:
            drv = json.load(f)
    report["driver"] = drv
    if proc.returncode != 0:
        report["reasons"].append("the driver failed (exit %d): see targets.log" % proc.returncode)
    ids = [c.get("id") for c in drv.get("cards", [])]
    want = [t for t in ("macos", "windows", "vita") if t in manifest["targets"]]
    if ids != want:
        report["reasons"].append("the selector shows %s, the manifest has %s" % (ids, want))
    for c in drv.get("cards", []):
        if not c.get("host_can_build") and (c.get("build_enabled") or not c.get("reason")):
            report["reasons"].append("target %s is not greyed with its reason" % c.get("id"))

    spec = manifest["targets"].get(a.target, {})
    if spec.get("unavailable_reason"):
        # the pinned source cannot build this target on any host (a lane not merged): the card must be greyed with the
        # reason -- checked above -- and the artefact leg is PENDING that merge, not RED
        report[a.target] = "PENDING the merge of the lane: " + spec["unavailable_reason"]
    elif a.target == "windows" and sys.platform != "win32":
        report["windows"] = "PENDING a Windows host: greyed here with the reason: " + str(next((c.get("reason") for c in drv.get("cards", []) if c.get("id") == "windows"), ""))
    elif drv.get("built"):
        art = drv.get("artifact", "")
        report["artifact"] = art
        report["artifact_bytes"] = drv.get("artifact_bytes")
        report["build_seconds"] = drv.get("seconds")
        if spec.get("artifact") == "vpk":
            with zipfile.ZipFile(art) as z:
                names = z.namelist()
            report["vpk_entries"] = names
            if "eboot.bin" not in names:
                report["reasons"].append("the .vpk has no eboot.bin")
            if not a.skip_vita3k:
                run = os.path.join(ROOT, "tools", "vita", "vita3k_run.sh")
                out = subprocess.run(["/bin/bash", run, "--vpk", art, "--seconds", str(a.vita3k_seconds), "--log-level", "2"],
                                     capture_output=True, text=True, timeout=a.vita3k_seconds + 120).stdout
                with open(os.path.join(a.out, "vita3k.log"), "w") as f:
                    f.write(out)
                state_name = manifest.get("run", {}).get("title_state_name", "Title (7)")
                reached = state_name in out or re.search(r"game state.*Title", out) is not None
                report["vita3k_title_reached"] = reached
                if not reached:
                    report["reasons"].append("the eboot did not reach %s in Vita3K (see vita3k.log)" % state_name)
        elif spec.get("artifact") == "zip":
            with zipfile.ZipFile(art) as z:
                report["zip_entries"] = z.namelist()[:200]
    else:
        report["reasons"].append("the %s target produced no artefact: %s" % (a.target, drv.get("failure") or drv.get("cross_reason") or "no record"))
    report["screenshots"] = sorted(os.listdir(shots)) if os.path.isdir(shots) else []
    report["result"] = "GREEN" if proc.returncode == 0 and not report["reasons"] else "RED"
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
    print("launcher_targets_judge: %s (%s) -- %s" % (report["result"], a.target, report.get("artifact") or report.get(a.target) or "no artefact"))
    for r in report["reasons"]:
        print("  - " + r)
    return 0 if report["result"] == "GREEN" else 1


if __name__ == "__main__":
    sys.exit(main())
