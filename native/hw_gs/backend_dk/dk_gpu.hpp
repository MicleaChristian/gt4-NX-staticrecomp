// TASK-174: deko3d device handle bundle for CoreGS on Switch (Vulkan-free).
// Mirrors hwgs::gpu::VulkanHandles so a future writeback_dk drawer can take the same shape.
#pragma once

#include <cstdint>
#include <string>

namespace hwgs::dk {

// Opaque deko3d objects stored as void* so this header stays free of <deko3d.h>
// for translation units that only forward the handles.
struct DkHandles {
  void *device = nullptr;   // DkDevice
  void *queue = nullptr;    // DkQueue
  void *cmdbuf = nullptr;   // DkCmdBuf
  std::uint32_t fb_width = 1280;
  std::uint32_t fb_height = 720;
};

// Creates a graphics-capable deko3d device+queue used by BackendDk / writeback_dk.
// Returns false with `error` filled on failure. No swapchain here — present stays in
// platform/nx/deko3d_present (staging copyBufferToImage) until scanout is GPU-resident.
bool create_device(DkHandles &out, std::string &error);
void destroy_device(DkHandles &handles);

// Upload tightly packed RGBA8 into the shared Switch present path.
bool present_rgba(const std::uint32_t *pixels, std::uint32_t width, std::uint32_t height,
                  std::uint64_t frame_osd);

} // namespace hwgs::dk
