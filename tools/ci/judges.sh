#!/usr/bin/env bash
# tools/ci/judges.sh -- the regression judges on one binary: every presented-frame / census judge under tools/judges with its
# frozen thresholds under fixtures/judges. Each judge was RED on the binary before its fix and GREEN after; here they are run
# GREEN-only on a candidate, one game process at a time, in three sets:
#
#   tools/ci/judges.sh --iso <disc image> [--bin BIN] [--build-dir DIR] [--out DIR] [--set standing|features|title|all]
#
#   standing   the hub's skinned characters, shadow volumes, the shadow kept off the caster, landing dust, particles, the
#              cryscave flicker, the frozen heap layout
#   features   the speedrun HUD, level select, the language table and tuple, the fps counter, the cavern crystals, the title
#              orb, mod saves namespaced, speedrun mode, the language setting, decals, display settings, the CT16 present,
#              the intro ocean seams (the fixture's frozen maximum; the oracle comparison needs the oracle dump), voice pitch
#   title      tools/judges/run_graphics_judges.sh: the self-test frames and the title's legal screen + world
#
# Prints one line per judge (rc, seconds, the verdict line); exit non-zero when any judge is RED. The hub card the voice
# pitch judge warps from is padlog_smoke's (build_all writes it); without it that judge is skipped.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ISO=""; BUILD="$ROOT/build"; BIN=""; OUT=""; SET=all
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --set) SET="$2"; shift 2 ;;
    *) echo "judges: unknown argument $1"; exit 2 ;;
  esac
done
[ -n "$ISO" ] || { echo "judges: --iso required"; exit 2; }
BIN="${BIN:-$BUILD/twinsanityevolved}"
[ -x "$BIN" ] || { echo "judges: no runner at $BIN"; exit 2; }
OUT="${OUT:-$BUILD/judges}"
mkdir -p "$OUT"
status=0
waitidle() { while pgrep -f '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null; do sleep 5; done; }
j() { # name judge [args...]  -- python3 tools/judges/<judge>.py --bin BIN --iso ISO --out DIR args
  local name=$1 judge=$2; shift 2
  local dir="$OUT/$name.d"; rm -rf "$dir"; mkdir -p "$dir"
  waitidle
  local t0; t0=$(date +%s)
  python3 "tools/judges/$judge.py" --bin "$BIN" --iso "$ISO" --out "$dir" "$@" > "$OUT/$name.log" 2>&1
  local rc=$?
  echo "judges: $name rc=$rc $(( $(date +%s) - t0 ))s $(grep -oE '(verdict )?(RED|GREEN).*' "$OUT/$name.log" | tail -1 | cut -c1-200)"
  [ "$rc" = 0 ] || status=1
  rm -rf "$dir"
}
standing() {
  j skin skin_judge --fixture fixtures/judges/hub_skin_pal.json
  j shadow shadow_judge --fixture fixtures/judges/hub_shadow_pal.json
  j shadow_caster shadow_caster_judge --fixture fixtures/judges/hub_shadow_caster_pal.json
  j landing_dust dust_judge --fixture fixtures/judges/landing_dust_pal.json
  j particles particle_judge --fixture fixtures/judges/hub_particles_pal.json
  j cryscave_flicker flicker_judge --fixture fixtures/judges/cavern_cryscave_flicker_pal.json
  j heap_layout heap_layout_judge
}
features() {
  j speedrun_hud speedrun_hud_judge
  j level_select level_select_judge
  j language_table language_table_judge
  j language_tuple language_tuple_judge --language english
  j fps_counter fps_counter_judge
  j cavern_crystals crystal_judge
  j title_orb orb_judge
  j mods_save mods_save_judge
  j speedrun speedrun_judge
  j language language_judge
  j decals decal_judge
  j display display_judge
  j present_ct16 present_ct16_judge
  j intro_ocean ocean_seam_judge --no-oracle
  if [ -d "$BUILD/padlog_smoke/mc_hub" ]; then
    local dir="$OUT/voice_pitch.d"; rm -rf "$dir"; mkdir -p "$dir"; waitidle
    local t0; t0=$(date +%s)
    python3 tools/judges/voice_pitch_judge.py --binary "$BIN" --iso "$ISO" --out "$dir" --hub-card "$BUILD/padlog_smoke/mc_hub" > "$OUT/voice_pitch.log" 2>&1
    local rc=$?
    echo "judges: voice_pitch rc=$rc $(( $(date +%s) - t0 ))s $(grep -E 'voice_pitch_judge: (RED|GREEN)' "$OUT/voice_pitch.log" | tail -1 | cut -c1-200)"
    [ "$rc" = 0 ] || status=1
    rm -rf "$dir"
  else
    echo "judges: voice_pitch SKIPPED (no hub card at $BUILD/padlog_smoke/mc_hub: run build_all or padlog_smoke first)"
  fi
}
title() {
  waitidle
  local t0; t0=$(date +%s)
  tools/judges/run_graphics_judges.sh --build "$(dirname "$BIN")" --iso "$ISO" > "$OUT/title.log" 2>&1
  local rc=$?
  echo "judges: title rc=$rc $(( $(date +%s) - t0 ))s $(grep -E '^(GREEN|RED|LEGAL|WORLD)' "$OUT/title.log" | tr '\n' ' ')"
  [ "$rc" = 0 ] || status=1
}
case "$SET" in
  standing) standing ;;
  features) features ;;
  title) title ;;
  all) standing; features; title ;;
  *) echo "judges: --set standing|features|title|all"; exit 2 ;;
esac
[ "$status" = 0 ] && echo "judges: ALL GREEN" || echo "judges: RED"
exit $status
