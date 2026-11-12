# Nintendo Switch homebrew NRO port (TASK-174)

Status: **playable-build plan, GPU renderer only** (owner, 2026-11-12). Primary objective per
[decision 0077](decisions/0077-switch-primary-target.md).

## Goal

On Atmosphere CFW Switch 1 the player copies one folder (`gt4recomp.nro`, `core.elf`) next to their own
**Gran Turismo 4 USA v2.00** plain `.iso`, launches the NRO, and plays: menus, races, replays, saves and sound, at
a steady frame rate. The ISO is data only; EE and VU1 code are compiled ahead of time to ARM64 on the PC. No JIT.
Renderer: CoreGS on deko3d. No GSdx.

**"Playable" gate (owner tests on device):** boot to the main menu without the CPU renderer; car select and menus
at 30 FPS or better; a Time Trial race on a common track at 30 FPS or better, picture correct (depth, textures,
HUD); sound; memory card save and load; Home and Plus exit cleanly. 60 FPS is the later target, not the gate.

## Decisions (2026-11-12)

- **The soft (CPU GS) build is dropped.** One export folder, `export/switch-gpu/`. CoreGS starts by default; nothing
  has to be enabled. `coregs.off` next to the NRO falls back to the CPU renderer for debugging only, and
  `gt4recomp.ini` no longer controls the renderer (old files saved `coregs=0`).
- **Measure before optimising.** Every phase ends with a device run whose `perf.log` names the next cost.

## What the first device logs showed (2026-11-12, run with CoreGS off by mistake)

`coregs.on` was not in `sdmc:/switch/GT4/`, so the run used the CPU renderer. That renderer cost 12 ms per frame at
boot and up to 150 ms in 3D menus (the log booked it under the VU1 interpreter column: a stale object kept old
slot numbers; the build now rebuilds every object when a shared header changes). Everything outside the renderer
(EE, IOP, VU0, events) cost a further 15-30 ms per frame, which by itself caps the game at roughly 30-50 FPS.
VU1 ran only 0-5 programs per frame in those menus. Evidence: `private/switch-device-logs/2026-11-12-run1-coregs-off/`.

## Phases

| Phase | Work | Gate (device) |
|---|---|---|
| **A. Truthful baseline** | GPU-only build; perf slots for EE, IOP, VU0, VU1, GIF front end, GPU draw, CPU fallback draw, GPU→GS-memory flush, compose, present; Switch audio output | A: `stderr.log` says `CoreGS BackendDk active`; `perf.log` from title, main menu, car select, a race |
| **B. Renderer completeness** | WritebackDk draws what GT4 uses: depth test and write, alpha test, texture function (modulate/decal/highlight), fog, remaining texture formats, so `cpu_fallback_pf` falls towards 0; correct picture in races | B: race picture correct; `gs_cpu_draw_ms` < 2 |
| **C. Stop the per-frame round trip** | Compose the display on the GPU from the colour target (no full GS-memory readback every frame); read back only regions the game reads (render-to-texture, flare reads) | C: `gs_flush_ms + gs_compose_ms` < 3 |
| **D. Use the other CPU cores** | Run VIF1/VU1/GIF/CoreGS on a second core (the donor already has a VU thread and a render queue); EE+IOP stay on the main core | D: menus and race ≥ 30 FPS |
| **E. CPU cost of the game itself** | VU0 micro AOT (14 known entries, as on PC slice 160), compiler flags (-O3, LTO) for EE units, IOP hot paths | E: race ≥ 45 FPS |
| **F. Polish** | 60 Hz pacing and frame hold, controls (analog triggers, button prompts), docked 1080p scaling, Home/sleep handling, save safety | Playable gate above |

Order: A → B → C → D → E → F. B and C can overlap once A's logs say which costs more.

## Layout on the SD card

```
sdmc:/switch/GT4/
  gt4recomp.nro
  core.elf
  Gran Turismo 4 (USA) (v2.00).iso
  saves/
  perf.log, session_summary.txt, stderr.log, halt.log   # written at runtime
```

Prefer an **exFAT** SD card for the dual-layer ISO. See [switch/README.md](../switch/README.md) and
[export/switch-gpu/PERF.md](../export/switch-gpu/PERF.md).

## Code map

- Platform seam: `native/platform/` (paths, clock; Win32 and `nx`).
- Present: [`native/platform/nx/deko3d_present.cpp`](../native/platform/nx/deko3d_present.cpp), fence ring + GPU blit.
- Renderer: [`native/hw_gs/backend_dk/`](../native/hw_gs/backend_dk/) (`BackendDk`, `WritebackDk`).
- VU1: AOT from `private/switch-vu1-gen` + overlay `private/switch-vu1-runtime/vu_micro.cpp`.
- IPU: `private/switch-ipu-runtime/ipu.cpp` (`native/ipu_fast.hpp`).
- App and glue: `switch/` (`game_main_nx.cpp`, `coregs_nx.cpp`, `perf_nx.cpp`, patch scripts in `switch/tools/`).

## Build

From `D:\gt4sr\switch` (junction to this checkout, no spaces):

```
make require SWITCH_EE_GENERATED=../private/switch-ee/generated -j4
cmd.exe /d /c switch\tools\export-switch.cmd
```

## Out of scope until the playable gate

Spec II, ultrawide, internal resolution above 1x, texture packs, Switch 2, a GT3-style native rewrite.
