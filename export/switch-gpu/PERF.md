# Device logs (copy these off the SD card after a run)

Next to `gt4recomp.nro` the NRO writes:

| File | What |
|---|---|
| `perf.log` | One line every 60 guest frames: FPS, frame jitter, time per subsystem, renderer counters |
| `session_summary.txt` | Whole-run averages, slots sorted largest first (rewritten every 10 lines and on exit) |
| `stderr.log` | Boot lines, renderer status, and a short `[perf]` line per window with the four biggest costs |
| `halt.log` | Why the run stopped |

`stderr.log` must say `CoreGS BackendDk active`. If it says `ERROR: CoreGS start failed` or `coregs.off`, the run
used the slow CPU renderer.

## What the time columns mean

Each `*_ms` column is milliseconds per guest frame spent only in that part (a nested part pauses its parent), so
together with `other_ms` they add up to `wall_ms`. 60 FPS needs `wall_ms` near 16.7.

| Column | Part |
|---|---|
| `ee_ms` | The game's own code (EE), plus DMA/IPU work it starts directly |
| `iop_ms` | IOP (sound, disc, memory card modules) |
| `vu0_ms` | VU0 micro programs |
| `vu1_aot_ms` / `vu1_interp_ms` | VU1 3D geometry, compiled vs interpreter fallback |
| `gif_ms` | Renderer front end: reading GS packets and building batches |
| `gs_gpu_draw_ms` | Recording draws for the Switch GPU |
| `gs_cpu_draw_ms` | Draws the GPU path refused and the CPU drew instead |
| `gs_flush_ms` | Copying the GPU picture back to GS memory (includes waiting for the GPU) |
| `gs_compose_ms` | Building the final display image on the CPU |
| `present_ms` | Putting the image on screen |
| `other_ms` | Everything outside a slot |

Counters: `cpu_fallback_pf` (batches drawn on the CPU per frame), `gpu_draws_pf`, `aot_hit_pct`, `gif_KiB_pf`.

Paste `perf.log`, `session_summary.txt` and `stderr.log` when reporting, and say which scenes you visited.
