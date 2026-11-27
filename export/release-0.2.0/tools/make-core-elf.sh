#!/usr/bin/env bash
# GT4Recomp NX 0.2.0: makes core.elf from YOUR OWN Gran Turismo 4 (USA, v2.00) disc image.
# Usage: bash make-core-elf.sh /path/to/game.iso
# Needs Python 3. Nothing is downloaded; the game data never leaves your computer.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
iso="${1:-}"
if [[ -z "$iso" || ! -f "$iso" ]]; then
  echo "usage: $0 <gran-turismo-4-usa-v2.00.iso>" >&2
  exit 2
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 -I "$here/disc.py" extract "$iso" "$work" CORE.GT4
python3 -I "$here/gt4_core.py" "$work/CORE.GT4" "$here/core.elf"
echo "Done: $here/core.elf"
echo "Copy core.elf to the SD card folder switch/GT4, next to gt4recomp.nro."
