#!/usr/bin/env python3
"""Patch donor system.cpp for the Switch NRO.

1. Re-boot reset: clear halt/frame/event state so A/X can run the game again in-NRO (g_halted is file-private).
2. perf.log slots: EE dispatch and IOP interpreter time (switch/include/perf_nx.hpp).
"""
from __future__ import annotations

import pathlib
import sys

BOOT_NEEDLE = "void boot(std::uint32_t entry, const std::vector<std::string> &args) {"
BOOT_INJECT = """void boot(std::uint32_t entry, const std::vector<std::string> &args) {
#if defined(__SWITCH__)
    // Donor assumes a fresh process. Clear halt/frame/event state so A/X can run again in-NRO.
    g_halted = false;
    g_halt_reason.clear();
    g_frames = 0;
    g_events.clear();
    g_base = 0;
    g_given = 0;
    g_iop_synced = 0;
    g_state_request = false;
    g_resumed = false;
    g_cpu = Cpu{};
#endif
"""

INCLUDE_NEEDLE = '#include "ps2/system.hpp"\n'
INCLUDE_INJECT = '#include "ps2/system.hpp"\n#include "perf_nx.hpp"\n'

IOP_NEEDLE = "        iop::run(t - g_iop_synced);\n"
IOP_INJECT = (
    "        {\n"
    "            gt4::perf::Scope perf_iop(gt4::perf::kIop);\n"
    "            iop::run(t - g_iop_synced);\n"
    "        }\n"
)

EE_NEEDLE = "        dispatch(c, 0, 0);\n"
EE_INJECT = (
    "        {\n"
    "            gt4::perf::Scope perf_ee(gt4::perf::kEe);\n"
    "            dispatch(c, 0, 0);\n"
    "        }\n"
)

PATCHES = [
    ("boot reset", BOOT_NEEDLE, BOOT_INJECT),
    ("perf include", INCLUDE_NEEDLE, INCLUDE_INJECT),
    ("iop slot", IOP_NEEDLE, IOP_INJECT),
    ("ee slot", EE_NEEDLE, EE_INJECT),
]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-system-boot.py <system.cpp> <out.cpp>", file=sys.stderr)
        return 2
    out = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    for name, needle, inject in PATCHES:
        count = out.count(needle)
        if count != 1:
            print(f"{name}: expected one anchor, found {count}", file=sys.stderr)
            return 1
        out = out.replace(needle, inject, 1)
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
