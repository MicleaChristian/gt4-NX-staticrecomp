# Device logs (copy these off the SD card after a run)

Next to `gt4recomp.nro` the NRO writes:

| File | What |
|---|---|
| `perf.log` | CSV, one line every 60 guest frames (FPS, slot times, AOT hit rate, GIF/GPU counters) |
| `session_summary.txt` | Averages for the whole run (written on exit / Plus abort) |
| `stderr.log` | Boot lines + a short `[perf]` mirror of each CSV window |
| `halt.log` | Why the run stopped |

## Reading `perf.log`

Header comments list every column. Useful ones:

- **fps / wall_ms** — host speed (target ~60 / ~16.7 ms).
- **frame_ms_min/avg/max/p95** — time between guest vblanks (jitter / latency).
- **vu1_aot_ms vs vu1_interp_ms** — VU1 AOT vs interpreter cost.
- **vu1_aot_hit_pct** — should be high once AOT is live; low means missing programs.
- **gif_ms / coregs_ms / soft_ms / present_ms / other_ms** — GIF, CoreGS compose, donor GS, host present, everything else (EE/IOP/IPU…).
- **gpu_draws_pf / cpu_fallback_pf** — WritebackDk draws vs CPU DrawBatch fallbacks.
- **present_w/h** — guest frame size handed to deko3d.

Highest `*_ms` column in a scene is the bottleneck. Paste `perf.log` + `session_summary.txt` (or just `stderr.log`) when reporting FPS.
