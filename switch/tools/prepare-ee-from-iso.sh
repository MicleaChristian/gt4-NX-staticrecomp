#!/usr/bin/env bash
# TASK-174 S3: from a GT4 USA v2.00 ISO on the PC, produce core.elf + generated EE C++ for the Switch NRO.
# Usage:
#   bash switch/tools/prepare-ee-from-iso.sh "/path/to/Gran Turismo 4 (USA) (v2.00).iso"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DONOR="$ROOT/private/references/ps2-web-recomp"
TOOLS="$DONOR/build/tools"
OUT="$ROOT/private/switch-ee"
ISO="${1:-}"

if [[ -z "$ISO" || ! -f "$ISO" ]]; then
  echo "usage: $0 <gt4-usa-v2.00.iso>" >&2
  exit 2
fi
if [[ ! -x "$TOOLS/eerecomp.exe" && ! -x "$TOOLS/eerecomp" ]]; then
  echo "building host eerecomp..."
  g++ -std=c++20 -O2 -Wall -o "$TOOLS/eerecomp.exe" \
    "$DONOR/recomp/main.cpp" "$DONOR/recomp/r5900.cpp" "$DONOR/recomp/elf.cpp" \
    "$DONOR/recomp/analysis.cpp" "$DONOR/recomp/codegen.cpp" "$DONOR/recomp/snr2.cpp"
fi
EERECOMP="$TOOLS/eerecomp.exe"
[[ -x "$TOOLS/eerecomp" ]] && EERECOMP="$TOOLS/eerecomp"

mkdir -p "$OUT/disc" "$OUT/generated"
python3 -I "$DONOR/scripts/disc.py" extract "$ISO" "$OUT/disc" SYSTEM.CNF >/dev/null
python3 -I "$DONOR/scripts/disc.py" extract "$ISO" "$OUT/disc" CORE.GT4 >/dev/null
python3 -I "$DONOR/scripts/gt4_core.py" "$OUT/disc/CORE.GT4" "$OUT/core.elf"
echo "ELF -> $OUT/core.elf"

# Static translation (no explore loop yet — fast first link). Re-run with explore later for coverage.
"$EERECOMP" "$OUT/core.elf" --out "$OUT/generated"
echo "units -> $OUT/generated ($(ls "$OUT/generated"/*.cpp 2>/dev/null | wc -l) cpp files)"
echo
echo "Next:"
echo "  1. set DEVKITPRO=D:/devkitPro"
echo "  2. make -C switch require SWITCH_EE_GENERATED=$OUT/generated"
echo "  3. copy $OUT/core.elf next to the NRO on the SD card (same folder as the ISO)"
