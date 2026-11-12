// TASK-174: CoreGS Dispatch lifecycle on Switch (BackendDk + deko3d present).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gt4::switch_app {

// Starts GT4_RENDERER=hw with BackendDk. Returns false if init fails (caller keeps donor GS).
bool coregs_start();
void coregs_stop();
bool coregs_active();

// Feed GIF PATH1..3 into CoreGS when active. Returns true if CoreGS consumed the packet
// (donor gs::render must be skipped).
bool coregs_gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc);

// Privileged GS register write (0x12000000..). Returns true if handled.
bool coregs_write_privileged(std::uint32_t address, std::uint64_t value);

// One guest VBlank: compose + present through deko3d.
// Returns true when a CoreGS frame was shown (caller skips donor display_frame blit).
bool coregs_vblank_present(std::uint64_t frame_osd);

// Fill GIF / GPU writeback counters for perf.log (no-op when CoreGS is off).
void coregs_fill_perf(std::uint64_t &gif_bytes, std::uint64_t &gpu_draws, std::uint64_t &gpu_batches,
                      std::uint64_t &cpu_fallback, std::uint64_t &tex_cache_hits, std::uint64_t &wb_flushes);

} // namespace gt4::switch_app
