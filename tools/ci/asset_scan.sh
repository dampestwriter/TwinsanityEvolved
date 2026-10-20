#!/usr/bin/env bash
# tools/ci/asset_scan.sh -- compatibility wrapper: the scan itself is tools/ci/asset_scan.py (Python, so the
# Windows lanes need no Git for Windows bash.exe -- the launcher's MinGit ships none). POSIX callers keep
# their documented entry point; the Windows scripts call the .py directly.
set -euo pipefail
exec python3 "${0%/*}/asset_scan.py" "$@"
