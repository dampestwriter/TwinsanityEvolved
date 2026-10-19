#!/usr/bin/env bash
# tools/ci/savestate_movie_roundtrip.sh -- the frozen judge of save states taken INSIDE a movie:
#   case A  the hub pad log (--skip-movies) saved at boundary 1710, inside GameController::StateMovie (the intro FMV,
#           skipped): the loaded run must leave the state at the same boundary and present the same frames to 1960.
#   case B  the boot movies playing (no --skip-movies), saved at boundary 1000 inside the TT ident FMV: the loaded run
#           must not crash (the fiber resumes inside MoviePlayer::WaitFrame's checkpoint), must present the same frames
#           to 1800 and pop the sound track with no silence.
# RED on main 6cd7dc0 (A: the movie player was not a save-state section -- Open failed through Find() after a load, the
# controller left StateMovie 3 boundaries early, 6 of 25 frames differed; B: SIGBUS -- WaitFrame held a Player* across
# the checkpoint), GREEN on the fix (platform/nativeee/host/movie_player.cpp: the kTagMovie section, Boundary re-finds).
#
#   tools/ci/savestate_movie_roundtrip.sh --iso <disc image> [--build-dir DIR] [--runner BIN] [--out DIR]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ISO=""; BUILD="$ROOT/build"; OUT=""; RUNNER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --runner) RUNNER="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "savestate_movie_roundtrip: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "savestate_movie_roundtrip: --iso required"; exit 2; }
RUNNER="${RUNNER:-$BUILD/twinsanityevolved}"
OUT="${OUT:-$BUILD/savestate_movie_roundtrip}"
rm -rf "$OUT"; mkdir -p "$OUT"
red=0

# run_case NAME SAVE STOP MIN_FRAMES [extra runner args...]
run_case() {
  local name=$1 save=$2 stop=$3 min_frames=$4; shift 4
  local dir="$OUT/$name"; mkdir -p "$dir/a" "$dir/b" "$dir/mc_a"
  echo "=== case $name: save at $save, compare to $stop ($*)"
  TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$dir/a" TE_GFX_DUMP_EVERY=10 \
    "$RUNNER" --iso "$ISO" --deterministic --no-sdl-input --no-audio --trace-state "$@" --mc-dir "$dir/mc_a" \
    --save-state "$dir/state.ps2st@$save" --stop-at "$stop" --run-seconds 240 > "$dir/a/run.log" 2>&1
  local rc=$?
  [ $rc = 0 ] || { echo "  RED: straight run exit $rc"; red=1; return; }
  [ -f "$dir/state.ps2st" ] || { echo "  RED: no state written"; red=1; return; }
  cp -R "$dir/mc_a" "$dir/mc_b"
  TE_GFX_HEADLESS=1 TE_GFX_DUMP_DIR="$dir/b" TE_GFX_DUMP_EVERY=10 \
    "$RUNNER" --iso "$ISO" --deterministic --no-sdl-input --no-audio --trace-state "$@" --mc-dir "$dir/mc_b" \
    --load-state "$dir/state.ps2st" --stop-at "$stop" --run-seconds 240 > "$dir/b/run.log" 2>&1
  rc=$?
  rm -f "$dir/state.ps2st"
  [ $rc = 0 ] || { echo "  RED: loaded run exit $rc (a crash: $(tail -1 "$dir/b/run.log" | cut -c1-120))"; red=1; return; }
  grep -q "savestate: loaded" "$dir/b/run.log" || { echo "  RED: the state was not loaded"; red=1; return; }
  # the state trace after the save (+1: the loaded run logs its current state once)
  grep "game state" "$dir/a/run.log" | sed 's/.*boundary //' | awk -F: -v s="$save" '$1+0 > s + 1' > "$dir/trace_a.txt"
  grep "game state" "$dir/b/run.log" | sed 's/.*boundary //' | awk -F: -v s="$save" '$1+0 > s + 1' > "$dir/trace_b.txt"
  if ! cmp -s "$dir/trace_a.txt" "$dir/trace_b.txt"; then echo "  RED: the state traces differ"; diff "$dir/trace_a.txt" "$dir/trace_b.txt" | head -6; red=1; fi
  local frames=0 differ=0
  for f in "$dir"/b/frame_*.ppm; do
    [ -e "$f" ] || continue
    local n; n=$(basename "$f")
    [ -f "$dir/a/$n" ] || continue
    frames=$((frames + 1))
    cmp -s "$dir/a/$n" "$f" || differ=$((differ + 1))
  done
  echo "  frames compared $frames, differing $differ"
  [ "$frames" -ge "$min_frames" ] || { echo "  RED: only $frames frames to compare (< $min_frames)"; red=1; }
  [ "$differ" = 0 ] || { echo "  RED: $differ frames differ"; red=1; }
  local census; census=$(grep "movie: constructs" "$dir/b/run.log" | tail -1 | sed 's/.*] //')
  echo "  loaded run: $census"
  case "$*" in *--skip-movies*) ;; *)
    echo "$census" | grep -qE "pcm frames=[1-9][0-9]* silence=0" || { echo "  RED: the loaded run's movie sound is not 'pcm frames>0 silence=0'"; red=1; }
    echo "$census" | grep -qE "presents=[1-9][0-9]*" || { echo "  RED: the loaded run presented no movie pictures"; red=1; } ;;
  esac
  grep -q "unknown=0" <<< "$census" || { echo "  RED: the loaded run had movie calls on an unknown player"; red=1; }
}

run_case skipped_movie 1710 1960 20 --skip-movies --play-pad "$ROOT/fixtures/padlogs/hub_entry.padlog"
run_case playing_movie 1000 1800 40

if [ $red = 0 ]; then echo "savestate_movie_roundtrip: GREEN"; exit 0; fi
echo "savestate_movie_roundtrip: RED"; exit 1
