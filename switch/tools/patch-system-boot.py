#!/usr/bin/env python3
"""Inject a Switch re-boot reset into donor system.cpp (g_halted is file-private)."""
from __future__ import annotations

import pathlib
import sys

NEEDLE = "void boot(std::uint32_t entry, const std::vector<std::string> &args) {"
INJECT = """void boot(std::uint32_t entry, const std::vector<std::string> &args) {
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


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch-system-boot.py <system.cpp> <out.cpp>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    marker = "Donor assumes a fresh process"
    if marker in src:
        pathlib.Path(sys.argv[2]).write_text(src, encoding="utf-8")
        print("already patched", sys.argv[2])
        return 0
    if NEEDLE not in src:
        print("boot() needle not found", file=sys.stderr)
        return 1
    out = src.replace(NEEDLE, INJECT, 1)
    if marker not in out:
        print("inject failed", file=sys.stderr)
        return 1
    pathlib.Path(sys.argv[2]).write_text(out, encoding="utf-8")
    print("patched", sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
