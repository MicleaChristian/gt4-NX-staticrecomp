// TASK-174 S4: Switch present contract.
// deko3d swapchain fed by copyBufferToImage from a CPU-letterboxed staging buffer;
// the libnx linear framebuffer is the fallback when deko3d cannot start.
#pragma once

#include <cstdint>
#include <cstddef>

namespace gt4::platform::nx {

struct PresentConfig {
  int scale = 1;
  bool performance_mode = true;
  int framebuffer_width = 1280;
  int framebuffer_height = 720;
};

bool deko3d_present_create(const PresentConfig &config);
void deko3d_present_destroy();

// Leave the text console and open a 1280x720 RGBA framebuffer for game frames.
// Call before ps2::run(); pair with soft_present_end() after.
bool soft_present_begin();
void soft_present_end();

// Upload tightly packed RGBA8 (uint32 little-endian R,G,B,A), letterboxed, and flip.
// frame_osd is unused (kept for call-site compatibility).
bool deko3d_present_frame(const std::uint8_t *rgba, std::size_t bytes, int width, int height,
                          std::uint64_t frame_osd);

// Black frame (used when the GS display circuit is still off).
bool soft_present_heartbeat(std::uint64_t frame);

} // namespace gt4::platform::nx
