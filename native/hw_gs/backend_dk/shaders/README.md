# WritebackDk shaders (UAM → DKSH)

| File | Stage | Role |
|---|---|---|
| `gs_draw_vsh.glsl` | vert | Pass-through NDC position, RGBA, UV |
| `gs_draw_fsh.glsl` | frag | `outColor = vColor * texture(tex, vUv)` with alpha discard |

`switch/runtime.mk` runs `uam` + `switch/tools/embed_dksh.py` into `switch/build/gs_draw_*_dksh.hpp`,
which `writeback_dk.cpp` includes.
