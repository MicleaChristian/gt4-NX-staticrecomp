#!/usr/bin/env bash
# Build donor eerecomp/vurecomp on the PC (MSYS2 gcc) for preparing EE units.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DONOR="$ROOT/private/references/ps2-web-recomp"
BUILD="$DONOR/build/tools"
mkdir -p "$BUILD"
cd "$DONOR"
cmake -S . -B "$BUILD" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target eerecomp vurecomp -j"$(nproc 2>/dev/null || echo 4)"
echo "built: $BUILD/eerecomp"
ls -la "$BUILD/eerecomp" "$BUILD/vurecomp"
