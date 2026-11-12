#!/usr/bin/env python3
"""Route donor gif::transfer to CoreGS BackendDk when active (Switch NRO)."""
from __future__ import annotations

import pathlib
import sys

INCLUDE_NEEDLE = '#include "gif.hpp"\n'
INCLUDE_INJECT = '#include "gif.hpp"\n#include "coregs_nx.hpp"\n#include "perf_nx.hpp"\n'

TRANSFER_NEEDLE = """void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
"""

TRANSFER_INJECT = """void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
#if defined(__SWITCH__)
    gt4::perf::Scope gif_scope(gt4::perf::kGif);
    // TASK-174: when CoreGS BackendDk is running, skip the donor CPU GS raster path.
    if (gt4::switch_app::coregs_gif_transfer(path, data, qwc)) {
        return;
    }
#endif
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
"""


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-gif-coregs.py <gif.cpp> <out.cpp>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    marker = "coregs_gif_transfer"
    if marker in src:
        pathlib.Path(sys.argv[2]).write_text(src, encoding="utf-8")
        print("already patched", sys.argv[2])
        return 0
    if INCLUDE_NEEDLE not in src:
        print("include needle not found", file=sys.stderr)
        return 1
    if TRANSFER_NEEDLE not in src:
        print("transfer() needle not found", file=sys.stderr)
        return 1
    out = src.replace(INCLUDE_NEEDLE, INCLUDE_INJECT, 1)
    out = out.replace(TRANSFER_NEEDLE, TRANSFER_INJECT, 1)
    if marker not in out:
        print("inject failed", file=sys.stderr)
        return 1
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
