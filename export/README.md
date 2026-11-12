# Export (Switch build)

One player folder: `export/switch-gpu/`. The NRO always uses the CoreGS GPU renderer (deko3d); nothing has to be
enabled. Each successful NRO build refreshes it in place.

## Copy to the Switch

Copy the folder's contents to the SD card, for example:

```text
sdmc:/switch/GT4/
  gt4recomp.nro
  core.elf
  *.iso                     your disc (not shipped here)
  saves/
```

Delete any old `coregs.on` there; it is no longer used. A `coregs.off` file switches back to the CPU renderer for
debugging only (very slow).

After a run, copy back `perf.log`, `session_summary.txt` and `stderr.log` (see `PERF.md`).

## Rebuild and refresh the export

```bat
set DEVKITPRO=D:\devkitPro
cd /d D:\gt4sr\switch
make require SWITCH_EE_GENERATED=../private/switch-ee/generated
switch\tools\export-switch.cmd
```
