#!/usr/bin/env bash
# Forwards to the Python implementation.
# Usage:
#   scripts/check_clang_format.sh                 # check vs origin/main
#   scripts/check_clang_format.sh --fix           # apply formatting fix
#   scripts/check_clang_format.sh --base HEAD~1   # check vs a different ref
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if command -v python3 >/dev/null 2>&1; then
    exec python3 "$SCRIPT_DIR/check_clang_format.py" "$@"
elif command -v python >/dev/null 2>&1; then
    exec python "$SCRIPT_DIR/check_clang_format.py" "$@"
else
    echo "[ERROR] Python not found in PATH." >&2
    exit 2
fi
