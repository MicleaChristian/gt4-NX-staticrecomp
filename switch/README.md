# GT4Recomp Switch homebrew (TASK-174)

Prebuilt ARM64 `.nro` for Atmosphere on **Switch 1**. Place it next to your own Gran Turismo 4 USA v2.00 plain `.iso`.
The NRO does **not** recompile the game on the console.

## SD card layout

```
sdmc:/switch/gt4recomp/
  gt4recomp.nro
  *.iso                 # your disc image (exFAT recommended; FAT32 max file is 4 GiB)
  saves/                # created at runtime for the memory card
  shaders/              # optional; used once the deko3d CoreGS path needs host shaders
```

Launch `gt4recomp.nro` from the homebrew menu. If no `.iso` is found next to the NRO, the app shows an error and exits
on Plus / Start.

## Build (DevKitPro)

1. Install [DevKitPro](https://devkitpro.org/) with the `switch-dev` package group (`libnx`, `devkitA64`).
2. Ensure `DEVKITPRO` is set (this machine: `D:\devkitPro`, **not** `/opt/devkitpro`).
3. Stub NRO (ISO find only):

```bat
set DEVKITPRO=D:\devkitPro
switch\tools\build-nro.cmd
```

4. **Full runtime NRO (S3):** copy your GT4 ISO onto the PC, then see
   [docs/switch/RUNTIME-LINK.md](../docs/switch/RUNTIME-LINK.md) —
   `prepare-ee-from-iso.sh` then `make require SWITCH_EE_GENERATED=../private/switch-ee/generated`.
   Also copy `private/switch-ee/core.elf` next to the NRO on the SD card.

Output objects stay under `switch/build/` (messy on purpose). **Player copy always goes to**
[`export/switch/`](../export/switch/) (soft GS) and [`export/switch-gpu/`](../export/switch-gpu/) (CoreGS)
— both refreshed after each successful NRO build (`switch/tools/export-switch.cmd`).
See [`export/README.md`](../export/README.md).

Optional packaging into a distributable folder:

```bat
switch\tools\package-sd.cmd
```

writes `switch/dist/gt4recomp/` with the NRO, README, and empty `saves/` / `shaders/` placeholders.

## Stages in this tree

| Piece | Role |
|---|---|
| `source/main.cpp` | Console init, ISO find, status UI, pad exit |
| `source/iso_find.*` | Sibling `*.iso` discovery |
| `source/runtime_bridge.*` | Future link to donor + EE/VU + `game_main` |
| `source/deko3d_present.*` | Console status UI; game frames use `native/platform/nx/deko3d_present` |
| `source/coregs_nx.*` | CoreGS Dispatch start/stop + GIF/vblank glue (BackendDk) |
| `../native/hw_gs/backend_dk/` | Switch CoreGS backend (cpuref draw + deko3d present / WritebackDk scaffold) |
| `source/audio_nx.*` | libnx audio sink stub for pacing |
| `source/pad_nx.*` | Joy-Con / Pro → PS2-style buttons |
| `source/settings_nx.*` | Minimal in-NRO settings (scale, Performance Mode) |

GSdx is not part of this binary.
