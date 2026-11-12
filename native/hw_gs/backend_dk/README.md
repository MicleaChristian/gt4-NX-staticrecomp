# CoreGS BackendDk (Switch / deko3d)

Switch-only implementation of `gt4::hw::Backend` (see `hw_gs_backend_dk.cpp`).

## Current stack

1. **Front end:** CoreGS GIF parser → batches (same as PC).
2. **Draw:** `WritebackDk` on deko3d (UAM `gs_draw_{v,f}sh.glsl` → DKSH) when the device initialises;
   batches fall back to `cpuref` `DrawBatch` on unsupported textures / overflows.
3. **Coherence:** colour target is seeded once per GPU run (recorded, no per-batch WaitIdle);
   dirty pixels flush to SLM once per vblank scanout; 16-slot texture ring.
4. **Present:** PCRTC compose from SLM (CPU), then `platform/nx/deko3d_present` (GPU scale blit,
   double-buffered staging + fences).

## Remaining fidelity / speed work

| Gap | Notes |
|---|---|
| Uber pixel shader | Full `hw_pixel_uber` TFX/ATE/Z/fog/dither not ported; current path is modulate + optional blend |
| Depth | No Z test/write on GPU yet |
| Compose on GPU | Still CPU `EndFrame` / PCRTC from SLM — next win after VU1 AOT |
| IMAGE→GPU | Host BITBLT still lands in SLM first; direct GPU upload for FMV/UI next |
| Texture formats | Indexed + colour formats via SLM decode; exotic PSMs fall back to CPU |

Build: linked from `switch/runtime.mk` when `SWITCH_EE_GENERATED` is set. Enable on device with
`coregs=1` in `gt4.ini` or a `coregs.on` file next to the NRO.
