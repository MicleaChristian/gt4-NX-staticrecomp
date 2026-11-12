#!/usr/bin/env python3
"""Mirror donor gs::write_privileged into CoreGS BackendDk when active (Switch NRO)."""
from __future__ import annotations

import pathlib
import sys

INCLUDE_NEEDLE = '#include "gs.hpp"\n'
# gs.cpp already includes many headers; inject after gs.hpp if present, else after the first include.
INCLUDE_INJECT = '#include "gs.hpp"\n#include "coregs_nx.hpp"\n'

WRITE_NEEDLE = "void write_privileged(std::uint32_t address, std::uint64_t value) {\n"
WRITE_INJECT = """void write_privileged(std::uint32_t address, std::uint64_t value) {
#if defined(__SWITCH__)
    // Keep donor CSR/display state, and mirror into CoreGS when BackendDk is running.
    (void)gt4::switch_app::coregs_write_privileged(address, value);
#endif
"""


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-gs-coregs.py <gs.cpp> <out.cpp>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    marker = "coregs_write_privileged"
    if marker in src:
        pathlib.Path(sys.argv[2]).write_text(src, encoding="utf-8")
        print("already patched", sys.argv[2])
        return 0
    if WRITE_NEEDLE not in src:
        print("write_privileged needle not found", file=sys.stderr)
        return 1
    out = src
    if INCLUDE_NEEDLE in out and "coregs_nx.hpp" not in out:
        out = out.replace(INCLUDE_NEEDLE, INCLUDE_INJECT, 1)
    elif "coregs_nx.hpp" not in out:
        # Fall back: prepend include after the license/includes block's first line that includes something.
        first = out.find('#include ')
        if first < 0:
            print("no include found to attach coregs_nx.hpp", file=sys.stderr)
            return 1
        line_end = out.find('\n', first)
        out = out[: line_end + 1] + '#include "coregs_nx.hpp"\n' + out[line_end + 1 :]
    out = out.replace(WRITE_NEEDLE, WRITE_INJECT, 1)
    if marker not in out:
        print("inject failed", file=sys.stderr)
        return 1
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
