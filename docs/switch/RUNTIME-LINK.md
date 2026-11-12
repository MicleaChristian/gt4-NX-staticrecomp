# Linking the game runtime into the Switch NRO (TASK-174 S3)

## Goal

Provide a strong `gt4_switch_game_main(const char *iso, const char *app_dir)` that boots the MIT
donor runtime (`ps2-web-recomp` @ `1b480879…`) with EE units translated offline from your GT4 ISO.

## What you need on the PC

1. **DevKitPro** with `DEVKITPRO=D:\devkitPro` (or your install path).
2. Your **Gran Turismo 4 USA v2.00** plain `.iso` **on the PC** (copy it off the SD card if needed).
3. Donor checkout (already cloned for this worktree):
   `private/references/ps2-web-recomp` @ `1b4808793a12c45125262fd2d5f50a9ec9f7c020`.
4. Host `eerecomp` (built once):
   `private/references/ps2-web-recomp/build/tools/eerecomp.exe`.

Windows `ps2game.lib` is **not** used. Units are re-translated for aarch64.

## Steps

### A. Translate the game on the PC

In DevKitPro MSYS2:

```bash
bash "/d/Projects/Gt4 ercomp switch/gt4-staticrecomp/switch/tools/prepare-ee-from-iso.sh" \
  "/d/path/to/Gran Turismo 4 (USA) (v2.00).iso"
```

Writes:

- `private/switch-ee/core.elf`
- `private/switch-ee/generated/*.cpp` (EE units)

### B. Build the full NRO

```bat
set DEVKITPRO=D:\devkitPro
set PATH=D:\devkitPro\msys2\usr\bin;D:\devkitPro\tools\bin;D:\devkitPro\devkitA64\bin;%PATH%
cd /d "D:\Projects\Gt4 ercomp switch\gt4-staticrecomp\switch"
make clean
make require SWITCH_EE_GENERATED=../private/switch-ee/generated
```

### C. On the SD card

```text
sdmc:/switch/GT4/          (or gt4recomp/)
  gt4recomp.nro
  Gran Turismo 4 (USA) (v2.00).iso
  core.elf                 ← from private/switch-ee/core.elf
  saves/                   ← created at runtime
```

## Present / CoreGS (2026-11-12)

The full NRO starts **CoreGS BackendDk** (`GT4_RENDERER=hw`, scale 1×) when init succeeds:

- GIF PATH1–3 and privileged GS writes feed CoreGS (`patch-gif-coregs.py` / `patch-gs-coregs.py`).
- Each guest VBlank composes via CoreGS cpuref and presents through deko3d staging `copyBufferToImage`.
- If CoreGS init fails, the donor CPU GS + present path remains (libnx FB or deko3d).

Expect menus/boot to work; dense races stay CPU-bound until `WritebackDk` grows the UAM uber-shader Execute
(`native/hw_gs/backend_dk/README.md`). Success for S3 was boot past ReadyStub; S4 wire-up is now in tree.

## Status on this machine (2026-11-11)

- Donor cloned and `eerecomp.exe` built.
- Switch Makefile + `runtime.mk` + `game_main_nx.cpp` ready.
- **Blocked on:** path to the GT4 ISO on the PC (only present on the Switch SD so far).
