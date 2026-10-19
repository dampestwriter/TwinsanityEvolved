#!/usr/bin/env bash
# tools/ci/asset_scan.sh -- NO GAME DATA IN THE TREE (charter). Zero findings or the gate fails.
#
# Scans every file of the port tree and the game/ tree by EXTENSION and by MAGIC: disc images (ISO9660 "CD001",
# raw .bin/.cue), executables (ELF/IRX), archives (zip, 7z, rar, gzip, xz, zstd, tar), textures/images (PNG, JPEG,
# GIF, BMP, DDS, TGA), audio (RIFF/WAVE, VAGp, Ogg, MP3/ID3), video (MPEG/PSS pack header, MP4 ftyp, AVI, MOV), PS2
# saves/savestates (.ps2st/.p2s/.psu/.mcd). Excluded: .git, build directories, the fetched toolchain, __pycache__.
#
# EXCLUDED PATHS (an explicit list, EXCLUDED_DIRS below): the engine/ tree is the vendored NativeEE engine
# with its own test fixtures (the GS swizzle-table .bin, atlas dumps) and its own asset gate
# (engine/tools/ci/asset_scan.py, run by the engine's workflows); it is NOT scanned by default -- its content is not part
# of the port's build output -- and `--include-engine` reports it informationally. game/ IS scanned: it is the
# decompilation's source and must hold no game data.
#
#   tools/ci/asset_scan.sh [--include-engine] [ROOT]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INCLUDE_ENGINE=0
for a in "$@"; do
  case "$a" in
    --include-engine) INCLUDE_ENGINE=1 ;;
    *) ROOT="$a" ;;
  esac
done
cd "$ROOT"

EXT_RE='\.(iso|bin|cue|img|mdf|nrg|elf|irx|erx|ps2st|p2s|psu|mcd|mc2|max|cbs|pss|vag|va[gp]|bd|bh|mb|mh|rm2|sm2|psf|tga|dds|png|jpg|jpeg|gif|bmp|wav|mp3|ogg|flac|mp4|avi|mov|mkv|m2v|zip|7z|rar|tar|gz|tgz|xz|zst|bz2)$'

# the explicit exclusion list: paths relative to ROOT (find -path form). The vendored engine/ tree is the only tree
# excluded; add a path here, never a pattern.
EXCLUDED_DIRS=( ./engine )
PRUNE=( -path ./.git -o -path './build*' -o -path ./tools/toolchain -o -path ./dist -o -name __pycache__ -o -path '*/.git' )
if [ "$INCLUDE_ENGINE" = 0 ]; then for d in "${EXCLUDED_DIRS[@]}"; do PRUNE+=( -o -path "$d" ); done; fi

findings=0
report() { findings=$((findings+1)); echo "FINDING: $1  ($2)"; }

size_of() { # bytes: GNU stat first (GNU's `stat -f` is --file-system and exits 0); BSD stat has no -c
  stat -c %s "$1" 2>/dev/null || stat -f %z "$1"
}

magic_of() { # file -> label or empty
  local f="$1" head
  head="$(head -c 16 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
  case "$head" in
    7f454c46*) echo "ELF/IRX executable"; return ;;
    89504e470d0a1a0a*) echo "PNG image"; return ;;
    ffd8ff*) echo "JPEG image"; return ;;
    474946383[79]61*) echo "GIF image"; return ;;
    424d*) ;;  # BMP: too weak alone ("BM" prefix); extension rule covers it
    44445320*) echo "DDS texture"; return ;;
    504b0304*|504b0506*) echo "ZIP archive"; return ;;
    377abcaf271c*) echo "7z archive"; return ;;
    526172211a07*) echo "RAR archive"; return ;;
    1f8b*) echo "gzip archive"; return ;;
    fd377a585a00*) echo "xz archive"; return ;;
    28b52ffd*) echo "zstd archive"; return ;;
    564147[70]*) echo "VAG audio"; return ;;
    4f676753*) echo "Ogg audio"; return ;;
    494433*) echo "MP3/ID3 audio"; return ;;
    000001ba*|000001b3*) echo "MPEG/PSS video"; return ;;
  esac
  case "$head" in
    52494646????????57415645*) echo "RIFF/WAVE audio"; return ;;
    52494646????????41564920*) echo "RIFF/AVI video"; return ;;
  esac
  local b4_12
  b4_12="$(head -c 12 "$f" 2>/dev/null | tail -c 8 | od -An -c | tr -d ' \n')"
  case "$b4_12" in *ftyp*) echo "MP4/MOV video"; return ;; esac
  # ISO9660: "CD001" at byte 0x8001 (plain) -- only worth checking on files >= 64 KB
  if [ "$(size_of "$f")" -ge 65536 ]; then
    if [ "$(dd if="$f" bs=1 skip=32769 count=5 2>/dev/null)" = "CD001" ]; then echo "ISO9660 disc image"; return; fi
  fi
  # tar: "ustar" at 257
  if [ "$(dd if="$f" bs=1 skip=257 count=5 2>/dev/null)" = "ustar" ]; then echo "tar archive"; return; fi
  echo ""
}

scanned=0
while IFS= read -r -d '' f; do
  scanned=$((scanned+1))
  rel="${f#./}"
  lower="$(printf '%s' "$rel" | tr 'A-Z' 'a-z')"
  # The ONE picture exemption: screenshots of the launcher's own window for README (docs/launcher/screenshots/
  # launcher-*.png, under 2 MB). The launcher shows no disc content -- its card artwork is generated from the title
  # id -- so these carry no game data; anything else under that folder, or larger, is still a finding.
  if [[ "$lower" =~ ^docs/launcher/screenshots/launcher-[a-z0-9-]+\.png$ ]] && [ "$(size_of "$f")" -le 2097152 ]; then continue; fi
  if [[ "$lower" =~ $EXT_RE ]]; then report "$rel" "forbidden extension"; continue; fi
  case "$(basename "$rel")" in SLUS_*|SLES_*|SCUS_*|SCES_*|IOPRP*) report "$rel" "PS2 executable name"; continue ;; esac
  m="$(magic_of "$f")"
  if [ -n "$m" ]; then report "$rel" "magic: $m"; fi
done < <(find . \( "${PRUNE[@]}" \) -prune -o -type f -print0)

echo "asset_scan: scanned $scanned files under $ROOT$( [ "$INCLUDE_ENGINE" = 1 ] && echo ' (engine included)' ), findings: $findings"
[ "$findings" -eq 0 ]
