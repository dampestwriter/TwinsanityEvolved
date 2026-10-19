#!/usr/bin/env bash
# tools/vita/vita3k_run.sh -- install the .vpk into Vita3K and run it headless-ish (the SDL frontend, console mode) for
# a bounded time, with a size watchdog on Vita3K's own log: an emulated fault that the emulator retries (an exclusive
# load on an unmapped address, for one) writes its error block tens of thousands of times a second and fills the disk
# in minutes. The app's own log is ux0:data/twinsanity/twinsanityevolved.log under Vita3K's emulated file system.
#
#   tools/vita/vita3k_run.sh [--vpk PATH] [--seconds N] [--log-level N] [--max-log-mb N]
#   env: VITA3K (the binary; default /Applications/Vita3K.app/Contents/MacOS/Vita3K)
#        VITA3K_PREF (Vita3K's data dir; default ~/Library/Application Support/Vita3K/Vita3K)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VPK="$ROOT/build-vita-release/twinsanityevolved.vpk"; SECONDS_MAX=90; LOG_LEVEL=4; MAX_LOG_MB=50
while [ $# -gt 0 ]; do
  case "$1" in
    --vpk) VPK="$2"; shift 2 ;;
    --seconds) SECONDS_MAX="$2"; shift 2 ;;
    --log-level) LOG_LEVEL="$2"; shift 2 ;;
    --max-log-mb) MAX_LOG_MB="$2"; shift 2 ;;
    *) echo "vita3k_run: unknown argument $1"; exit 2 ;;
  esac
done
VITA3K="${VITA3K:-/Applications/Vita3K.app/Contents/MacOS/Vita3K}"
PREF="${VITA3K_PREF:-$HOME/Library/Application Support/Vita3K/Vita3K}"
APPLOG="$PREF/fs/ux0/data/twinsanity/twinsanityevolved.log"
[ -x "$VITA3K" ] || { echo "vita3k_run: $VITA3K not found"; exit 2; }
[ -f "$VPK" ] || { echo "vita3k_run: $VPK not found (build the vita-release preset first)"; exit 2; }
if pgrep -f '(^|/)(twinsanityevolved|ps2recomp|ps2recomp_oracle|ps2recomp_release[.0-9a-f]*|Vita3K)( |$)' > /dev/null; then
  echo "vita3k_run: a game process (or Vita3K) is running; one at a time"; exit 3
fi
: > "$PREF/vita3k.log"
rm -f "$APPLOG"
"$VITA3K" --frontend sdl -z --log-level "$LOG_LEVEL" "$VPK" > /tmp/vita3k_run.out 2>&1 &
PID=$!
start=$(date +%s); reason="exited"
while kill -0 "$PID" 2>/dev/null; do
  now=$(date +%s)
  if [ $((now - start)) -ge "$SECONDS_MAX" ]; then reason="timeout ${SECONDS_MAX}s"; kill "$PID"; sleep 1; kill -9 "$PID" 2>/dev/null; break; fi
  size_mb=$(( $(stat -f %z "$PREF/vita3k.log" 2>/dev/null || echo 0) / 1048576 ))
  if [ "$size_mb" -ge "$MAX_LOG_MB" ]; then reason="vita3k.log > ${MAX_LOG_MB} MB (an emulated fault loop); killed"; kill -9 "$PID"; break; fi
  sleep 1
done
wait "$PID" 2>/dev/null; rc=$?
echo "vita3k_run: $reason (exit $rc) after $(( $(date +%s) - start )) s"
echo "--- vita3k.log (tail) ---"; grep -v "mvk-info\|^\s*VK_\|^\t" "$PREF/vita3k.log" | tail -12 | cut -c1-200
echo "--- host stderr (tail) ---"; grep -v "mvk-info\|^\s*VK_\|^\t" /tmp/vita3k_run.out | tail -5 | cut -c1-200
echo "--- app log: $APPLOG ---"; [ -f "$APPLOG" ] && cat "$APPLOG" | cut -c1-220 | tail -60 || echo "(none)"
