# Export (Switch builds)

Two folders, same NRO binary — CoreGS is selected by `coregs.on` next to the NRO.

| Folder | Path | What it is |
|---|---|---|
| **Soft** | `export/switch/` | Donor CPU GS + deko3d present (no `coregs.on`) |
| **GPU** | `export/switch-gpu/` | CoreGS BackendDk + WritebackDk (`coregs.on` included) |

Each successful NRO build refreshes **both** in place.

## Copy to the Switch

Pick one folder and copy it to the SD card, for example:

```text
sdmc:/switch/GT4/          ← soft, or
sdmc:/switch/GT4-gpu/      ← GPU
  gt4recomp.nro
  core.elf
  coregs.on                ← only in the GPU folder
  *.iso                    ← your disc (not shipped here)
  saves/
```

## Rebuild → refresh export

```bat
set DEVKITPRO=D:\devkitPro
cd /d D:\gt4sr\switch
make require SWITCH_EE_GENERATED=../private/switch-ee/generated
```

Or:

```bat
switch\tools\export-switch.cmd
```
