#!/usr/bin/env python3
"""Ensure Switch perf_nx.hpp is visible to the (possibly VU1-AOT-overlayed) vu_micro.cpp,
and export AOT counters for perf.log."""
from __future__ import annotations

import pathlib
import sys

INCLUDE_NEEDLE = '#include "ps2/vu.hpp"\n'
INCLUDE_INJECT = '#include "ps2/vu.hpp"\n#include "perf_nx.hpp"\n'

SNAPSHOT_MARKER = "gt4_vu1_aot_snapshot"
SNAPSHOT_INJECT = r'''
// Switch perf.log: cumulative AOT counters (compiled / interpreted / identity rejects).
extern "C" void gt4_vu1_aot_snapshot(std::uint64_t *compiled, std::uint64_t *interpreted,
                                     std::uint64_t *identity_rejects) {
    if (compiled) *compiled = vu1aot::g_counters.compiled;
    if (interpreted) *interpreted = vu1aot::g_counters.interpreted;
    if (identity_rejects) *identity_rejects = vu1aot::g_counters.identity_rejects;
}
'''


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-vu-perf.py <vu_micro.cpp> <out.cpp>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    out = src
    if "perf_nx.hpp" not in out:
        if INCLUDE_NEEDLE not in out:
            print("include needle not found", file=sys.stderr)
            return 1
        out = out.replace(INCLUDE_NEEDLE, INCLUDE_INJECT, 1)
    # aarch64: strtoull returns unsigned long long; uint64_t may be unsigned long.
    bad = (
        "    static const std::uint64_t report_every = [] {\n"
        '        const char *period = std::getenv("GT4_VU1_REPORT_EVERY");\n'
        "        if (period) return std::strtoull(period, nullptr, 10);\n"
        '        return std::getenv("GT4_PERF") ? std::uint64_t{4096} : std::uint64_t{0};\n'
        "    }();"
    )
    good = (
        "    static const std::uint64_t report_every = []() -> std::uint64_t {\n"
        '        const char *period = std::getenv("GT4_VU1_REPORT_EVERY");\n'
        "        if (period) return static_cast<std::uint64_t>(std::strtoull(period, nullptr, 10));\n"
        '        return std::getenv("GT4_PERF") ? std::uint64_t{4096} : std::uint64_t{0};\n'
        "    }();"
    )
    if bad in out:
        out = out.replace(bad, good, 1)
    if SNAPSHOT_MARKER not in out and "Counters g_counters;" in out:
        out = out.replace("Counters g_counters;", "Counters g_counters;" + SNAPSHOT_INJECT, 1)
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
