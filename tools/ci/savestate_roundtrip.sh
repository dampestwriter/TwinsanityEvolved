#!/usr/bin/env bash
# tools/ci/savestate_roundtrip.sh -- the save-state gate and the hub pad-log gate, on a disc image:
#
#   1. straight run: --play-pad fixtures/padlogs/hub_entry.padlog to boundary STOP, frames dumped every 10 presents,
#      --save-state at boundary SAVE on the way; the run must reach MainMenu (8), NewGame (10) and Playing (12) and
#      create the save file (icon.sys + Bank0.bin under the test mc dir);
#   2. loaded run: --load-state of that file, the same pad log, to STOP;
#   3. the GameController state trace after SAVE and every presented frame dumped by both runs after SAVE must be
#      byte-identical (deterministic clock: progress is a pure function of boundaries, so a loaded game IS the saved one).
#
#   tools/ci/savestate_roundtrip.sh --iso <disc image> [--build-dir DIR] [--save N] [--stop N] [--out DIR]
#
# Exit non-zero on the first failing check. Game data is read at run time only; the state file and the frames go to
# --out (a scratch directory, default under the build dir) and are not committed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ISO=""; BUILD="$ROOT/build"; SAVE=2380; STOP=2600; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --save) SAVE="$2"; shift 2 ;;
    --stop) STOP="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "savestate_roundtrip: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "savestate_roundtrip: --iso required"; exit 2; }
OUT="${OUT:-$BUILD/savestate_roundtrip}"
rm -rf "$OUT"; mkdir -p "$OUT/a" "$OUT/b" "$OUT/mc_a" "$OUT/mc_b"
PADLOG="$ROOT/fixtures/padlogs/hub_entry.padlog"
RUNNER="$BUILD/twinsanityevolved"
# --skip-movies: the pad logs were recorded against the movie stub (Title at 559); with the movie Player the boot movies
# move the title to ~1682 and the intro FMV plays on New Game, so the logs' boundaries hold only with movies skipped
# (a log recorded with the movies playing is the next padlog, same file format)
COMMON=(--iso "$ISO" --deterministic --no-sdl-input --no-audio --trace-state --skip-movies --play-pad "$PADLOG" --stop-at "$STOP" --run-seconds 240)

echo "=== straight run (save at $SAVE, stop at $STOP)"
TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$OUT/a" TE_GFX_DUMP_EVERY=10 \
  "$RUNNER" "${COMMON[@]}" --mc-dir "$OUT/mc_a" --save-state "$OUT/state.ps2st@$SAVE" > "$OUT/a/run.log" 2>&1 || { echo "savestate_roundtrip: straight run exit $?"; tail -5 "$OUT/a/run.log"; exit 1; }
grep -E "savestate:|clock:|audio: banks|stream:" "$OUT/a/run.log" | grep -v "foreign word" || true
for state in "MainMenu (8)" "NewGame (10)" "Playing (12)"; do
  grep -q "game state @ boundary [0-9]*: $state" "$OUT/a/run.log" || { echo "savestate_roundtrip: the pad log did not reach $state"; exit 1; }
done
[ -f "$OUT/state.ps2st" ] || { echo "savestate_roundtrip: no state file written"; exit 1; }
[ -f "$OUT/mc_a/mc0/BESLES-52568/icon.sys" ] && [ -f "$OUT/mc_a/mc0/BESLES-52568/Bank0.bin" ] || { echo "savestate_roundtrip: the game did not create its save file (saves.h)"; ls -R "$OUT/mc_a" || true; exit 1; }
echo "save file: $(ls "$OUT/mc_a/mc0/BESLES-52568" | tr '\n' ' ')"

echo "=== loaded run (from boundary $SAVE to $STOP)"
cp -R "$OUT/mc_a/." "$OUT/mc_b/"
TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$OUT/b" TE_GFX_DUMP_EVERY=10 \
  "$RUNNER" "${COMMON[@]}" --mc-dir "$OUT/mc_b" --load-state "$OUT/state.ps2st" > "$OUT/b/run.log" 2>&1 || { echo "savestate_roundtrip: loaded run exit $?"; tail -5 "$OUT/b/run.log"; exit 1; }
grep -E "savestate:|clock:|audio: banks|stream:" "$OUT/b/run.log" || true
grep -q "savestate: loaded" "$OUT/b/run.log" || { echo "savestate_roundtrip: the state was not loaded"; exit 1; }

echo "=== compare: state trace after $SAVE (+1: the loaded run logs its current state once), frames after $SAVE"
grep "game state" "$OUT/a/run.log" | sed 's/.*boundary //' | awk -F: -v s="$SAVE" '$1+0 > s + 1' > "$OUT/trace_a.txt"
grep "game state" "$OUT/b/run.log" | sed 's/.*boundary //' | awk -F: -v s="$SAVE" '$1+0 > s + 1' > "$OUT/trace_b.txt"
if ! cmp -s "$OUT/trace_a.txt" "$OUT/trace_b.txt"; then echo "savestate_roundtrip: the state traces differ"; diff "$OUT/trace_a.txt" "$OUT/trace_b.txt" | head; exit 1; fi
echo "state trace identical ($(wc -l < "$OUT/trace_a.txt") transitions after $SAVE)"
frames=0; differ=0
for f in "$OUT"/b/frame_*.ppm; do
  [ -e "$f" ] || continue
  n=$(basename "$f")
  [ -f "$OUT/a/$n" ] || continue
  frames=$((frames + 1))
  cmp -s "$OUT/a/$n" "$f" || { differ=$((differ + 1)); echo "  $n DIFFERS"; }
done
[ "$frames" -ge 3 ] || { echo "savestate_roundtrip: only $frames frames to compare"; exit 1; }
[ "$differ" = 0 ] || { echo "savestate_roundtrip: $differ of $frames frames differ"; exit 1; }
echo "$frames frames identical"
for key in "clock:" "audio: banks" "stream:"; do
  # prepare_max_ms is a wall-clock measurement (the longest music-window read), not state: it differs run to run
  a=$(grep "$key" "$OUT/a/run.log" | tail -1 | sed 's/.*] //; s/prepare_max_ms=[0-9.]*/prepare_max_ms=-/')
  b=$(grep "$key" "$OUT/b/run.log" | tail -1 | sed 's/.*] //; s/prepare_max_ms=[0-9.]*/prepare_max_ms=-/')
  [ "$a" = "$b" ] || { echo "savestate_roundtrip: census '$key' differs:"; echo "  a: $a"; echo "  b: $b"; exit 1; }
done
echo "census lines identical"
echo "savestate_roundtrip: PASS"
