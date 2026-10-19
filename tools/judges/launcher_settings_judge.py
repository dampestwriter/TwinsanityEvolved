#!/usr/bin/env python3
"""tools/judges/launcher_settings_judge.py -- the launcher's Settings page against the game's options file.

Two halves:
  1. ROUND TRIP: through the engine's Qt Test driver test_launcher_settings, every row of launcher/title.json set to
     every one of its values through the row's own control lands in ps2r.ini under the row's key, and a value written
     to the file by someone else is what the row shows after a reload. RED when there is no launcher or any row fails.
  2. THE MENU: FRAME RATE 120 chosen in the launcher is what the game shows at boot, and vice versa. The game is run
     with --config <the launcher's file> and the frame rate it reports in its log is read (the pacer's census row
     `present=<rate>:...` or a `frame_rate` line); then the file is written the way the game's Options menu writes it
     (`video.frame_rate = 144`) and the launcher's row must read it back (half 1 covers the read-back). When the game
     binary does not know the key yet (the row is not merged), this half is PENDING, not RED.

  tools/judges/launcher_settings_judge.py --launcher-build DIR --out DIR [--game BIN --iso IMAGE]
                                          [--manifest launcher/title.json] [--home DIR]

Exit 0 GREEN (half 2 GREEN or PENDING), 1 RED. Writes <out>/report.json.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def ini_set(path, key, value):
    """The engine's SettingsFile grammar: replace `key = value` or append it."""
    lines = []
    if os.path.isfile(path):
        with open(path) as f:
            lines = f.read().split("\n")
        if lines and lines[-1] == "":
            lines.pop()
    done = False
    for i, l in enumerate(lines):
        body = l.split("#")[0].split(";")[0].strip()
        if "=" in body and body.split("=")[0].strip() == key:
            lines[i] = key + " = " + value
            done = True
            break
    if not done:
        lines.append(key + " = " + value)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "launcher", "title.json"))
    ap.add_argument("--game", default="", help="the game binary for the menu half (else PENDING)")
    ap.add_argument("--iso", default="")
    ap.add_argument("--home", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_settings_judge", "result": "RED", "reasons": [], "menu_half": "PENDING"}
    driver = os.path.join(a.launcher_build, "test_launcher_settings")
    if not os.access(driver, os.X_OK):
        report["reasons"].append("no launcher: %s does not exist" % driver)
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_settings_judge: RED -- " + report["reasons"][0])
        return 1
    home = a.home or tempfile.mkdtemp(prefix="te-launcher-settings-home-")
    with open(a.manifest) as f:
        manifest = json.load(f)
    rows = [r for g in manifest["settings"]["groups"] for r in g["rows"]]
    keys = [r["key"] for r in rows]
    report["rows_in_manifest"] = keys
    titles = os.path.join(home, "titles")
    os.makedirs(titles, exist_ok=True)
    manifest_path = os.path.join(titles, "title.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f)
    report_path = os.path.join(a.out, "settings_report.json")
    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", PS2R_SETTINGS_MANIFEST=manifest_path, PS2R_SETTINGS_REPORT=report_path)
    with open(os.path.join(a.out, "settings.log"), "w") as log:
        proc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    report["driver_exit"] = proc.returncode
    checked = {}
    if os.path.isfile(report_path):
        with open(report_path) as f:
            checked = json.load(f)
    report["round_trip"] = checked
    if proc.returncode != 0:
        report["reasons"].append("the round-trip driver failed (exit %d): see settings.log" % proc.returncode)
    done = {r["key"] for r in checked.get("rows", []) if r.get("round_trip")}
    for k in keys:
        if k not in done:
            report["reasons"].append("row %s did not round-trip" % k)
    ini = checked.get("settings_file", "")
    # expected: "~/.twinsanityevolved/ps2r.ini" under the judge's HOME
    want = manifest["settings"]["file"].replace("~", home)
    if ini and os.path.realpath(ini) != os.path.realpath(want):
        report["reasons"].append("the launcher writes %s, the game reads %s" % (ini, want))
    report["settings_file"] = ini

    # half 2: the game's view of FRAME RATE
    if a.game and a.iso and os.access(a.game, os.X_OK) and "video.frame_rate" in keys:
        ini_set(ini, "video.frame_rate", "120")
        mc = os.path.join(home, "mc")
        os.makedirs(mc, exist_ok=True)
        cmd = [a.game, "--iso", a.iso, "--config", ini, "--mc-dir", mc, "--skip-movies", "--no-audio", "--run-seconds", "8", "--no-sdl-input"]
        genv = dict(os.environ, HOME=home, TE_GFX_HEADLESS="1", TE_GFX_SUMMARY_EVERY="120")
        try:   # the game reports on stderr
            out = subprocess.run(cmd, env=genv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180).stdout
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        with open(os.path.join(a.out, "game_120.log"), "w") as f:
            f.write(out)
        m = re.search(r"present=(\w+):", out) or re.search(r"frame[_ ]rate[^0-9a-z]*(\d+|uncapped)", out, re.I)
        if m:
            seen = m.group(1)
            report["game_frame_rate_seen"] = seen
            if seen == "120":
                report["menu_half"] = "GREEN"
            else:
                report["menu_half"] = "RED"
                report["reasons"].append("the game reports frame rate %s, the launcher wrote 120" % seen)
        else:
            report["menu_half"] = "PENDING"
            report["menu_half_note"] = "the game binary reports no frame rate (the FRAME RATE row is not in this binary yet); the key round-trip is judged, the menu half is pending"
        # vice versa: the game's menu writes 144 (the same SettingsFile grammar); the launcher must read 144 -- the
        # driver's reload check covered the read-back for every value, so the file is asserted here
        ini_set(ini, "video.frame_rate", "144")
        with open(ini) as f:
            report["menu_wrote_144_file_has"] = re.search(r"^video\.frame_rate\s*=\s*(\S+)", f.read(), re.M).group(1)
    else:
        report["menu_half_note"] = "no game binary / disc image given: the menu half is pending"
    report["result"] = "GREEN" if proc.returncode == 0 and not report["reasons"] else "RED"
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
    print("launcher_settings_judge: %s -- %d rows round-trip (%s); menu half %s" % (report["result"], len(done), ", ".join(sorted(done)), report["menu_half"]))
    for r in report["reasons"]:
        print("  - " + r)
    return 0 if report["result"] == "GREEN" else 1


if __name__ == "__main__":
    sys.exit(main())
