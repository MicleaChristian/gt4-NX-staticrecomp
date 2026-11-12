#!/usr/bin/env python3
"""Compile a GLSL shader with uam and emit a C header with the DKSH bytes."""
from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: embed_dksh.py <uam> <stage> <input.glsl> <output.hpp>", file=sys.stderr)
        return 2
    uam, stage, src, out = sys.argv[1:5]
    src_path = pathlib.Path(src)
    out_path = pathlib.Path(out)
    dksh = out_path.with_suffix(".dksh")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([uam, "-s", stage, "-o", str(dksh), str(src_path)])
    data = dksh.read_bytes()
    symbol = out_path.stem.replace(".", "_").replace("-", "_")
    lines = [
        f"// Generated from {src_path.name} — do not edit.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        f"inline constexpr std::size_t {symbol}_size = {len(data)};",
        f"inline constexpr std::uint8_t {symbol}_bytes[{len(data)}] = {{",
    ]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 12])
        lines.append(f"  {chunk},")
    lines.append("};")
    lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out_path} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
