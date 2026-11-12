#!/usr/bin/env bash
# TASK-174 S6: assemble sdmc:/switch/gt4recomp layout under switch/dist/gt4recomp
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/gt4recomp"
mkdir -p "$DIST/saves" "$DIST/shaders"
if [[ -f "$ROOT/build/gt4recomp.nro" ]]; then
  cp -f "$ROOT/build/gt4recomp.nro" "$DIST/gt4recomp.nro"
else
  echo "warning: NRO not built yet; packaging placeholders only" >&2
fi
cp -f "$ROOT/README.md" "$DIST/README.md"
: > "$DIST/saves/.keep"
: > "$DIST/shaders/.keep"
printf '%s\n' "Place your Gran Turismo 4 USA v2.00 plain .iso in this folder." > "$DIST/PUT_ISO_HERE.txt"
echo "Packaged: $DIST"
