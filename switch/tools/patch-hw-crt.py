#!/usr/bin/env python3
"""Forward the donor SetGsCrt syscall (hw::gs_set_crt) to CoreGS on the Switch NRO.

The donor only records the CRT mode; CoreGS needs SMODE1/SYNCV/SMODE2 to size the display
(NTSC field mode reads 224 lines per field, not 448).
"""
from __future__ import annotations

import pathlib
import sys

MARKER = "coregs_set_crt"
FUNC_NEEDLE = "void gs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {\n"
FUNC_INJECT = FUNC_NEEDLE + """#if defined(__SWITCH__)
    gt4::switch_app::coregs_set_crt(interlace, mode, field);
#endif
"""


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-hw-crt.py <hw.cpp> <out.cpp>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    if MARKER in src:
        print("hw.cpp already references coregs_set_crt", file=sys.stderr)
        return 1
    if src.count(FUNC_NEEDLE) != 1:
        print("gs_set_crt needle must match exactly once", file=sys.stderr)
        return 1
    first_include = src.find("#include ")
    if first_include < 0:
        print("no include found to attach coregs_nx.hpp", file=sys.stderr)
        return 1
    line_end = src.find("\n", first_include)
    out = src[: line_end + 1] + '#include "coregs_nx.hpp"\n' + src[line_end + 1 :]
    out = out.replace(FUNC_NEEDLE, FUNC_INJECT, 1)
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
