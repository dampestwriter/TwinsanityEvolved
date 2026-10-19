#!/usr/bin/env python3
"""root_mesh_level_entry.py -- RootMeshLevelEntry: the owned mesh bucket (hle.graphics_meshes)
draws the first level's certified type-01 placed meshes in the PRESENTED frame, and the
PATH1 geometry those materials sent through VU1 no longer reaches the GS state machine.

Two headless 7600-frame Release runs of the first-level route (RootShadowLevelEntry's padlog:
title -> menu -> the beach; the deterministic fields stated one by one, --profile default, a fresh
memory card per run) on the all-on composition:

  owned   the compiled default: every certified material's placed instances are collected at
          SetPlacedModelDMA, its material captured at FlushMaterial and its draws recorded at the
          bucket's slot of the ordered output (census graphics_meshes placed_replaced>0
          flush_replaced>0 draws>0 faults=0; graphics_mesh_materials certified>0 unknown_record=0
          unhashed=0 refused_*=0; graphics_refusals_meshes faults=0; graphics_output_faulted=0).
  model   --set hle.graphics_meshes=off: the title's writers run for every instance
          (placed_replaced=0, draws=0) -- the before state.

Retirement: the owned arm's gs_sink_path1_by_bucket kicks for buckets 2+3 (TR BucketOpaque /
BucketGlobalOpaque) are at most OPAQUE_KICKS_MAX_SHARE of the model arm's (measured 0.925: the
certificate's named residue -- every rigid instance, 0 rigid IDs certified under the HUD rule,
-- and the placed materials it refuses as shared consumers), and the owned arm's
bucket-2 kicks are strictly fewer. On the measured route the owned and model arms' frames.fhash
and audio.ahash are IDENTICAL line for line (the owned renderer reproduces the model's pixels).

Presented frame p<PIN_FRAME> (Crash idle on the beach): over the MESH_BOXES (the placed objects
the owned path draws there: the stone totem at the right edge, the crate at the left) the owned
frame matches the model arm's within the 5-bit floor -- at least MATCH_SHARE_MIN of the pixels
with every channel within FLOOR_LEVELS (8 = one 5-bit step; measured: identical) -- and Crash's
body box stays the same scene (the skin draws are not this owner's). Red-before, both seen on
the way here: an owned arm that collected the instances and never delivered them (the first
composition: the FlushMaterial wrapper overwritten) leaves the objects out of the frame, and an
owned arm sampling a mip-incomplete chain (TextureAssets::Upload's FinishMipChain off by one)
drew them BLACK (level_m2: totem box share within the floor 0.130) -- both fail the match judge;
a model arm with draws>0 fails the census judge.

Usage: --binary --game-files --runner [--frames N] [--timeout S] [--keep]; --selftest judges
fake frames / fake run.meta with no game process.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (the leg mechanics, the ppm reader, counters)

FRAMES = 7600
PIN_FRAME = 7000
PADLOG = shadow.PADLOG
# Top-down logical 512x448 boxes (x0, x1, y0, y1): the placed meshes the owned path draws on the
# pin frame -- the stone totem at the right edge and the wooden crate at the left -- and Crash.
MESH_BOXES = {"totem": (404, 512, 0, 240), "crate": (10, 130, 225, 275)}
BODY_BOX = shadow.BODY_BOX
FLOOR_LEVELS = 8          # the 5-bit floor: one step of a 5-bit channel on the 8-bit frame
MATCH_SHARE_MIN = 0.97    # share of box pixels within the floor (owned vs model)
BODY_SHARE_MAX = 0.10     # Crash's body box: the same scene (differing share)
# The owned arm's buckets 2+3 kicks as a share of the model arm's. MEASURED 0.925 (2026-10-12,
# level_m3: 2+3 = 368,416,514 vs 398,075,152; bucket 2 268.3 M vs 297.9 M, bucket 3 unchanged):
# the certificate owns 2,089 placed resource IDs and 0 rigid (the HUD rule), so the
# residue is every rigid instance (437,936 on the route) and the 546,164 shared-consumer placed
# materials it refuses -- the bound holds the owned share, it does not claim the retirement.
OPAQUE_KICKS_MAX_SHARE = 0.95
ARMS = ("owned", "model")
ARM_SETTINGS = {"owned": [], "model": ["hle.graphics_meshes=off"]}


def box_pixels(path, box):
    w, h, rows = shadow.read_ppm(path, flip=False)   # the presented image is top-down
    x0, x1, y0, y1 = shadow.scaled(box, w / 512.0, h / 448.0)
    return w, h, rows, (x0, x1, y0, y1)


def frame_match(path_a, path_b, box, floor):
    """(share of pixels with every channel within `floor`, share of differing pixels) over the
    box of two presented frames; None when the sizes differ."""
    wa, ha, ra, (x0, x1, y0, y1) = box_pixels(path_a, box)
    wb, hb, rb, _ = box_pixels(path_b, box)
    if (wa, ha) != (wb, hb):
        return None
    n = within = differing = 0
    for y in range(y0, y1):
        row_a, row_b = ra[y], rb[y]
        for x in range(x0, x1):
            i = x * 3
            d = max(abs(row_a[i] - row_b[i]), abs(row_a[i + 1] - row_b[i + 1]), abs(row_a[i + 2] - row_b[i + 2]))
            n += 1
            if d <= floor:
                within += 1
            if d:
                differing += 1
    return within / float(max(1, n)), differing / float(max(1, n))


def bucket_kicks(meta):
    """gs_sink_path1_by_bucket=exact=1,<bucket>:tags/kicks,... -> {bucket: kicks}."""
    out = {}
    for part in meta.get("gs_sink_path1_by_bucket", "").split(","):
        if ":" not in part or "/" not in part:
            continue
        bucket, counts = part.split(":", 1)
        try:
            out[int(bucket)] = int(counts.split("/")[1])
        except ValueError:
            continue
    return out


def judge_arm(name, returncode, meta, frames, frame_path):
    results = []

    def check(ok, what):
        results.append((bool(ok), "%s: %s" % (name, what)))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s)" % meta.get("park", "none"))
    check(meta.get("video") == "gl", "video=gl (video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("graphics_output_faulted", "0") == "0", "graphics_output_faulted=0")
    textures = shadow.counters(meta.get("graphics_textures", ""))
    check(textures.get("loaded", 0) > 0, "graphics_textures loaded>0 (textures at load; %s)"
          % meta.get("graphics_textures", "<absent>")[:60])
    row = meta.get("graphics_meshes", "<absent>")
    m = shadow.counters(row)
    materials_row = meta.get("graphics_mesh_materials", "<absent>")
    mm = shadow.counters(materials_row)
    refusals = shadow.counters(meta.get("graphics_refusals_meshes", ""))
    check(m.get("faults", 1) == 0 and refusals.get("faults", 1) == 0,
          "graphics_meshes faults=0 and graphics_refusals_meshes faults=0 (%s)" % row[-40:])
    if name == "owned":
        check(m.get("selected", 0) == 1, "graphics_meshes selected=1 (%s)" % row[:40])
        check(m.get("placed_calls", 0) > 0 and m.get("placed_replaced", 0) > 0 and m.get("flush_replaced", 0) > 0
              and m.get("draws", 0) > 0,
              "graphics_meshes placed_replaced>0 flush_replaced>0 draws>0 (the owned bucket executes; %s)" % row)
        check(mm.get("receipts", 0) > 0 and mm.get("unhashed", 1) == 0 and mm.get("certified", 0) > 0
              and mm.get("unknown_record", 1) == 0 and mm.get("unverified_archive", 1) == 0,
              "graphics_mesh_materials receipts>0 unhashed=0 certified>0 unknown_record=0 (the live record digests "
              "are the catalogue's; %s)" % materials_row)
        check(mm.get("reserved", 0) > 0 and mm.get("refused_environment", 1) == 0 and mm.get("refused_view", 1) == 0
              and mm.get("refused_slot", 1) == 0 and mm.get("texture_missing", 1) == 0 and mm.get("texture_mismatch", 1) == 0,
              "graphics_mesh_materials reserved>0 and no reservation/texture refusal (%s)" % materials_row[-120:])
    else:
        check(m.get("selected", 1) == 0 and m.get("placed_replaced", 1) == 0 and m.get("flush_replaced", 1) == 0
              and m.get("draws", 1) == 0 and m.get("placed_model_calls", 0) > 0,
              "model: graphics_meshes selected=0 placed_replaced=0 draws=0 placed_model_calls>0 (the writers run; %s)" % row)
    check(os.path.isfile(frame_path), "presented frame p%d dumped (%s)" % (PIN_FRAME, frame_path))
    return results


def judge_retirement(owned_meta, model_meta):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    owned, model = bucket_kicks(owned_meta), bucket_kicks(model_meta)
    model_opaque = model.get(2, 0) + model.get(3, 0)
    owned_opaque = owned.get(2, 0) + owned.get(3, 0)
    check(model_opaque > 0 and model.get(2, 0) > 0,
          "model arm: gs_sink_path1_by_bucket buckets 2+3 kicks>0 (the before state; 2:%d 3:%d)" % (model.get(2, 0), model.get(3, 0)))
    check(model_opaque > 0 and owned_opaque <= OPAQUE_KICKS_MAX_SHARE * model_opaque,
          "owned arm: buckets 2+3 kicks %d <= %.2f x model %d (share %.3f; the residue is the certificate's: rigid + shared consumers)"
          % (owned_opaque, OPAQUE_KICKS_MAX_SHARE, model_opaque, owned_opaque / float(max(1, model_opaque))))
    check(owned.get(2, 0) < model.get(2, 0),
          "owned arm: bucket 2 kicks %d < model %d" % (owned.get(2, 0), model.get(2, 0)))
    return results


def judge_frames(owned_path, model_path):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    if not (os.path.isfile(owned_path) and os.path.isfile(model_path)):
        check(False, "both presented frames present for the owned/model comparison")
        return results
    for name, box in MESH_BOXES.items():
        match = frame_match(owned_path, model_path, box, FLOOR_LEVELS)
        if match is None:
            check(False, "owned and model presented frames have one size")
            return results
        within, differing = match
        check(within >= MATCH_SHARE_MIN,
              "presented frame p%d %s box: the owned draw matches the model within the 5-bit floor "
              "(share within %d levels %.3f >= %.2f; differing share %.3f)"
              % (PIN_FRAME, name, FLOOR_LEVELS, within, MATCH_SHARE_MIN, differing))
    body = frame_match(owned_path, model_path, BODY_BOX, FLOOR_LEVELS)
    check(body is not None and body[1] <= BODY_SHARE_MAX,
          "presented frame p%d body box: the same scene (Crash's skin draws; differing share %.3f <= %.2f)"
          % (PIN_FRAME, body[1] if body else 1.0, BODY_SHARE_MAX))
    return results


META_KEYS = ("frames", "finalized", "park", "video", "graphics_output_faulted", "graphics_textures", "graphics_meshes",
             "graphics_mesh_assets", "graphics_mesh_materials", "graphics_refusals_meshes", "gs_sink_path1_by_bucket",
             "gif_path2_by_class", "gs_sink")


def fake_meta(name, replaced=5000, draws=20000, faults=0, certified=3000, unknown_record=0, refused_slot=0,
              bucket2=1000000, bucket3=300000):
    owned = name == "owned"
    if not owned:
        replaced, certified = 0, 0   # `draws` stays the caller's (a model arm that drew natively is a case)
    return {"frames": str(FRAMES), "finalized": "1", "park": "none", "video": "gl", "graphics_output_faulted": "0",
            "graphics_textures": "loaded=1100,destroyed=300,model_calls=0,refused=0,failed=0",
            "graphics_meshes": "selected=%d,rigid_calls=9000,rigid_replaced=0,rigid_model_calls=9000,rigid_interior=0,"
                               "placed_calls=14000,placed_replaced=%d,placed_model_calls=%d,placed_interior=0,"
                               "flush_calls=9000,flush_replaced=%d,flush_model_calls=%d,flush_interior=0,"
                               "direct_calls=0,direct_replaced=0,direct_model_calls=0,direct_interior=0,"
                               "blocks=%d,instances=%d,draws=%d,faults=%d"
                               % (owned, replaced, 14000 - replaced, replaced // 3, 9000 - replaced // 3,
                                  replaced // 2, replaced, draws, faults),
            "graphics_mesh_assets": "model_reads=643,loaded=643,invalid=0,unsupported=0,limit=0,destroys=0,lazy_loads=0,lazy_misses=0,live=643",
            "graphics_mesh_materials": "reads=612,receipts=%d,unhashed=0,live=576,owns_calls=9000,certified=%d,no_receipt=0,"
                                       "unknown_material=0,unknown_record=%d,unowned_consumer=6000,unverified_archive=0,"
                                       "texture_disagrees=0,bucket_declined=0,texture_lookups=3000,texture_missing=0,"
                                       "texture_mismatch=0,requests=%d,reserved=%d,refused_environment=0,refused_view=0,refused_slot=%d"
                                       % (612 if owned else 0, certified if owned else 0, unknown_record,
                                          replaced // 3, replaced // 3 - refused_slot, refused_slot),
            "graphics_refusals_meshes": "total=6000,missing_resource=0,unsupported_state=6000,ordering=0,faults=%d" % faults,
            "gs_sink_path1_by_bucket": "exact=1,2:%d/%d,3:%d/%d,6:100/3000" % (bucket2 // 30, bucket2, bucket3 // 30, bucket3)}


def fake_frame(path, totem, crate, totem_shift=0):
    """A beach frame: sky, a far green band, sand, a stone totem at the right edge (totem: drawn;
    else sand/sky where it stood), a wooden crate at the left (crate: drawn), Crash's fur in the
    body box. totem_shift adds a small per-channel offset inside the totem (a renderer's rounding)."""
    def pixel(x, y):
        if 404 <= x < 512 and 0 <= y < 240 and totem:
            v = 100 + ((x // 6 + y // 6) & 1) * 30 + totem_shift
            return (v, v + 4, v + 40)
        if 10 <= x < 130 and 225 <= y < 275 and crate:
            return (170 + ((x // 8) & 1) * 20, 95, 40)
        if y < 150:
            return (120, 170, 230)
        if y < 200:
            return (60, 120, 40) if (x // 4) & 1 else (120, 180, 80)
        if 238 <= x < 270 and 290 <= y < 322:
            return (125, 45, 4)
        return (230, 205, 150)
    shadow.write_ppm(path, 512, 448, pixel, flip=False)


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_mesh_level_entry_selftest_")
    try:
        model, owned, missing_totem, rounded, moved = (os.path.join(tmp, n) for n in
                                                       ("model.ppm", "owned.ppm", "missing.ppm", "rounded.ppm", "moved.ppm"))
        fake_frame(model, True, True)
        fake_frame(owned, True, True)
        fake_frame(missing_totem, False, True)       # red-before: collected, never delivered
        fake_frame(rounded, True, True, totem_shift=5)   # a renderer's rounding inside the floor
        shadow.write_ppm(moved, 512, 448, lambda x, y: fake_pixel_moved(x, y), flip=False)
        good_owned, good_model = fake_meta("owned", bucket2=300000, bucket3=300000), fake_meta("model", draws=0)
        cases = [
            ("good owned", lambda: judge_arm("owned", 0, good_owned, FRAMES, owned), None),
            ("good model", lambda: judge_arm("model", 0, good_model, FRAMES, model), None),
            ("good retirement", lambda: judge_retirement(good_owned, good_model), None),
            ("good frames", lambda: judge_frames(owned, model), None),
            ("frames within the floor", lambda: judge_frames(rounded, model), None),
            ("red-before: the totem never delivered", lambda: judge_frames(missing_totem, model), "totem box"),
            ("red-before: the owned arm replaced nothing", lambda: judge_arm("owned", 0, fake_meta("owned", replaced=0, draws=0), FRAMES, owned), "placed_replaced>0"),
            ("red-before: the owned arm sent as many kicks", lambda: judge_retirement(fake_meta("owned", bucket2=990000), good_model), "buckets 2+3 kicks"),
            ("the model arm's writers never ran", lambda: judge_arm("model", 0, dict(good_model, graphics_meshes=good_model["graphics_meshes"].replace("placed_model_calls=14000", "placed_model_calls=0")), FRAMES, model), "placed_model_calls>0"),
            ("a model arm that drew natively", lambda: judge_arm("model", 0, fake_meta("model", draws=5), FRAMES, model), "draws=0"),
            ("good model draws nothing", lambda: judge_arm("model", 0, fake_meta("model", draws=0), FRAMES, model), None),
            ("a record the catalogue does not know", lambda: judge_arm("owned", 0, fake_meta("owned", unknown_record=7, bucket2=300000), FRAMES, owned), "unknown_record=0"),
            ("a refused reservation", lambda: judge_arm("owned", 0, fake_meta("owned", refused_slot=1, bucket2=300000), FRAMES, owned), "no reservation/texture refusal"),
            ("a fault", lambda: judge_arm("owned", 0, fake_meta("owned", faults=1, bucket2=300000), FRAMES, owned), "faults=0"),
            ("a crash", lambda: judge_arm("owned", 134, dict(good_owned, finalized="0"), FRAMES, owned), "exits 0"),
            ("no presented frame", lambda: judge_arm("owned", 0, good_owned, FRAMES, os.path.join(tmp, "none.ppm")), "dumped"),
            ("a frame missing for the comparison", lambda: judge_frames(owned, os.path.join(tmp, "none2.ppm")), "both presented frames"),
            ("the body box changed (another scene)", lambda: judge_frames(moved, model), "the same scene"),
        ]
        for name, run, expect_fail in cases:
            results = run()
            failed = [what for ok, what in results if not ok]
            ok = (not failed) if expect_fail is None else any(expect_fail in what for what in failed)
            print("%s selftest: %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else " -> " + "; ".join(failed or ["(all passed)"])))
            failures += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootMeshLevelEntrySelftest: %s" % ("PASS" if not failures else "FAIL (%d)" % failures))
    return 1 if failures else 0


def fake_pixel_moved(x, y):
    """The good frame with Crash gone from the body box (another scene)."""
    if 404 <= x < 512 and 0 <= y < 240:
        v = 100 + ((x // 6 + y // 6) & 1) * 30
        return (v, v + 4, v + 40)
    if 10 <= x < 130 and 225 <= y < 275:
        return (170 + ((x // 8) & 1) * 20, 95, 40)
    if y < 150:
        return (120, 170, 230)
    if y < 200:
        return (60, 120, 40) if (x // 4) & 1 else (120, 180, 80)
    return (230, 205, 150)


def run_leg(args, name, settings, frames_dir, out_dir):
    log = out_dir + ".log"
    os.makedirs(frames_dir, exist_ok=True)
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", os.path.abspath(PADLOG), "--dump-frames", frames_dir, "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.user_data=" + os.path.join(out_dir, "user_data"), "--set", "video.scale=1"]
    for field in shadow.DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
    for setting in settings:
        cmd += ["--set", setting]
    keeper = shadow.FrameKeeper(frames_dir, (PIN_FRAME,))
    keeper.start()
    print("run[%s]: %s" % (name, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    keeper.stop.set()
    keeper.join()
    print("runner[%s] exit=%d" % (name, proc.returncode))
    meta_path = os.path.join(out_dir, "run.meta")
    meta = shadow.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in META_KEYS:
        print("run.meta[%s] %s=%s" % (name, key, meta.get(key, "<absent>")))
    return proc.returncode, meta, log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner")
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--out", help="keep the two legs under this directory (implies --keep)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.binary or not args.game_files or not args.runner:
        ap.error("--binary, --game-files and --runner are required (or --selftest)")
    if not os.path.isfile(os.path.join(args.game_files, shadow.ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77
    if not os.path.isfile(PADLOG):
        print("FAIL: route padlog missing: %s" % PADLOG)
        return 1
    root = args.out or tempfile.mkdtemp(prefix="root_mesh_level_entry_")
    if args.out:
        os.makedirs(root, exist_ok=True)
        args.keep = True
    failures = 0
    logs = []
    frame_paths, metas = {}, {}
    for name in ARMS:
        out_dir = os.path.join(root, name)
        frames_dir = os.path.join(out_dir, "frames")
        rc, meta, log = run_leg(args, name, ARM_SETTINGS[name], frames_dir, out_dir)
        logs.append(log)
        metas[name] = meta
        frame_paths[name] = os.path.join(frames_dir, "p%d.ppm" % PIN_FRAME)
        for ok, what in judge_arm(name, rc, meta, args.frames, frame_paths[name]):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
    for ok, what in judge_retirement(metas["owned"], metas["model"]):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    for ok, what in judge_frames(frame_paths["owned"], frame_paths["model"]):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootMeshLevelEntry: FAIL (%d) -- logs %s, out %s" % (failures, ", ".join(logs), root))
        return 1
    print("RootMeshLevelEntry: ALL PASS")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
