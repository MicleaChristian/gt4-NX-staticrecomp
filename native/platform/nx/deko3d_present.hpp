// TASK-174 S4: Switch present contract.
// Interim path = libnx software framebuffer blit of donor CPU-raster frames.
// Real deko3d / CoreGS swapchain replaces the blit later; the call sites stay the same.
#pragma once

#include <cstdint>
#include <cstddef>

namespace gt4::platform::nx {

struct PresentConfig {
  int scale = 1;
  bool performance_mode = true;
  // Guest-sized FB: the Switch compositor scales it. Blitting into 1280x720 on the CPU
  // every frame was a hard wall for FMV and menus (~900k nearest-neighbour writes).
  int framebuffer_width = 640;
  int framebuffer_height = 448;
};

bool deko3d_present_create(const PresentConfig &config);
void deko3d_present_destroy();

// Leave the text console and open a 1280x720 RGBA framebuffer for game frames.
// Call before ps2::run(); pair with soft_present_end() after.
bool soft_present_begin();
void soft_present_end();

// Upload tightly packed RGBA8 (uint32 little-endian R,G,B,A) and flip.
// Always draws a cyan frame OSD so a black GS does not look "frozen".
bool deko3d_present_frame(const std::uint8_t *rgba, std::size_t bytes, int width, int height,
                          std::uint64_t frame_osd);

// Black frame + cyan OSD (used when GS display circuit is still off).
bool soft_present_heartbeat(std::uint64_t frame);

} // namespace gt4::platform::nx
