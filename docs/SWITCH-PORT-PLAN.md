# Nintendo Switch homebrew NRO port (TASK-174)

Status: **60 FPS plan in progress** (2026-11-12). Primary objective per
[decision 0077](decisions/0077-switch-primary-target.md).

## Goal

On Atmosphere CFW Switch 1, the player puts `gt4recomp.nro` (and companion data) plus their own
**Gran Turismo 4 USA v2.00** plain `.iso` under e.g. `sdmc:/switch/gt4recomp/`, launches the NRO,
and the game boots. The ISO is **data only**. EE/VU guest code is AOT C++ compiled to **ARM64**
offline. No JIT. CoreGS + **deko3d** only; no GSdx.

## Layout on the SD card

```
sdmc:/switch/gt4recomp/          # or export/switch-gpu/ on the SD card
  gt4recomp.nro
  core.elf
  coregs.on                      # GPU / CoreGS build
  Gran Turismo 4.iso
  shaders/
  saves/
  perf.log                       # written at runtime (one line / 60 guest frames)
  stderr.log
```

Prefer an **exFAT** SD card for a full dual-layer ISO. See [switch/README.md](../switch/README.md).

## 60 FPS phases and gates

GT3-revisited’s daytime 60 FPS is a **native GLES rewrite**, not a template for this recomp. Only
deko host patterns transfer (rings, fences, GPU-resident frames). GT4 keeps EE/VU/GIF/GS.

| Phase | Work | Gate |
|---|---|---|
| **0** | `perf.log` / `stderr.log` on device | G0: lines show which slot owns the frame |
| **1** | Present ring: double-buffer staging, fences, GPU scale blit; no per-frame WaitIdle | G1: SCEA splash ≥30 FPS, no cyan OSD |
| **2** | VU1 AOT table linked into the NRO (`private/switch-vu1-gen`, overlayed `vu_micro`) | G3: main menu / car select ≥55 FPS |
| **3** | `ipu_fast` donor overlay (`private/switch-ipu-runtime`) | G2: intro / FMV ≥30 FPS |
| **4** | WritebackDk: deferred seed, one SLM flush per vblank; GPU PCRTC / IMAGE→GPU next | G3 solid |
| **5** | Race lock: 480p force, Time Trial ≥55 FPS; soft GS is baseline only | G4 |

Default device build: **`export/switch-gpu/`** (`coregs.on`). Soft folder = regression baseline.

## Platform seam

`native/platform/` — paths, clock, env, ISO discovery. Win32 + `nx` backends.

## Renderer

- **Present:** [`native/platform/nx/deko3d_present.cpp`](../native/platform/nx/deko3d_present.cpp) —
  guest RGBA → GPU blit/letterbox → swapchain; libnx FB fallback.
- **Backend:** [`native/hw_gs/backend_dk/`](../native/hw_gs/backend_dk/) (`BackendDk`).
- **VU1:** AOT from `private/switch-vu1-gen` + overlay in `private/switch-vu1-runtime/vu_micro.cpp`.
- **IPU:** `private/switch-ipu-runtime/ipu.cpp` (`native/ipu_fast.hpp`).

## Build

See [switch/README.md](../switch/README.md). From `D:\gt4sr\switch` (junction):

```
make require SWITCH_EE_GENERATED=../private/switch-ee/generated -j4
```

## Out of scope until v2.00 races

Spec II, ultrawide, high IR, texture packs, Tauri, GSdx, Switch 2, GT3-style native rewrite.
