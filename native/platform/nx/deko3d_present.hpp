// TASK-174 S4: Switch present contract.
// Interim path = libnx software framebuffer blit of donor CPU-raster frames.
// Real deko3d / CoreGS swapchain replaces the blit later; the call sites stay the same.
// Host present stamps a compile-time build/candidate ID (dark bar + cyan glyphs) onto the
// framebuffer after the guest blit; guest simulation is untouched.
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
// Host stamps compile-time build ID OSD after the guest pixels (not guest HUD).
bool deko3d_present_frame(const std::uint8_t *rgba, std::size_t bytes, int width, int height,
                          std::uint64_t frame_osd);

// Black frame + host build-ID stamp (used when GS display circuit is still off).
bool soft_present_heartbeat(std::uint64_t frame);

// Zero-copy GPU present (TASK-174 present milestone). The CoreGS renderer hands over its
// DkDevice/DkQueue (not owned here); present then owns a swapchain on that device and each
// scanout is a GPU blit from the renderer's colour image -- no readback, no WaitIdle.
// deko3d_present_frame / soft_present_heartbeat keep working (CPU rows -> staging -> blit).
bool deko3d_present_attach_gpu(void *dk_device, void *dk_queue);
void deko3d_present_detach_gpu();
bool deko3d_present_gpu_attached();
// dk_image: const DkImage* in the attached device. Rect is scaled to the whole framebuffer.
// NX-2026-11-27-BF: short text shown after the build ID in the top-left stamp of GPU presents
// (the frame rate). May be called from any thread; the stamp is redrawn at the next present.
void deko3d_present_set_status(const char *text);
bool deko3d_present_gpu_image(const void *dk_image, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                              std::uint32_t h);

} // namespace gt4::platform::nx
