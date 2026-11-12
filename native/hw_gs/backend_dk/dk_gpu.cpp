#include "dk_gpu.hpp"

#include "platform/nx/deko3d_present.hpp"

#if defined(__SWITCH__)
#include <deko3d.h>
#endif

namespace hwgs::dk {
namespace {
#if defined(__SWITCH__)
DkMemBlock g_cmd_mem = nullptr;
#endif
} // namespace

bool create_device(DkHandles &out, std::string &error) {
  destroy_device(out);
#if defined(__SWITCH__)
  DkDeviceMaker deviceMaker;
  dkDeviceMakerDefaults(&deviceMaker);
  DkDevice device = dkDeviceCreate(&deviceMaker);
  if (!device) {
    error = "dkDeviceCreate failed";
    return false;
  }

  DkMemBlockMaker memMaker;
  dkMemBlockMakerDefaults(&memMaker, device, 64 * 1024);
  memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g_cmd_mem = dkMemBlockCreate(&memMaker);
  if (!g_cmd_mem) {
    dkDeviceDestroy(device);
    error = "dkMemBlockCreate(cmd) failed";
    return false;
  }

  DkCmdBufMaker cmdMaker;
  dkCmdBufMakerDefaults(&cmdMaker, device);
  DkCmdBuf cmdbuf = dkCmdBufCreate(&cmdMaker);
  if (!cmdbuf) {
    dkMemBlockDestroy(g_cmd_mem);
    g_cmd_mem = nullptr;
    dkDeviceDestroy(device);
    error = "dkCmdBufCreate failed";
    return false;
  }
  dkCmdBufAddMemory(cmdbuf, g_cmd_mem, 0, 64 * 1024);

  DkQueueMaker queueMaker;
  dkQueueMakerDefaults(&queueMaker, device);
  queueMaker.flags = DkQueueFlags_Graphics;
  DkQueue queue = dkQueueCreate(&queueMaker);
  if (!queue) {
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(g_cmd_mem);
    g_cmd_mem = nullptr;
    dkDeviceDestroy(device);
    error = "dkQueueCreate failed";
    return false;
  }

  out.device = device;
  out.queue = queue;
  out.cmdbuf = cmdbuf;
  out.fb_width = 1280;
  out.fb_height = 720;
  return true;
#else
  (void)out;
  error = "deko3d only available on __SWITCH__";
  return false;
#endif
}

void destroy_device(DkHandles &handles) {
#if defined(__SWITCH__)
  if (handles.queue) {
    dkQueueWaitIdle(static_cast<DkQueue>(handles.queue));
    dkQueueDestroy(static_cast<DkQueue>(handles.queue));
  }
  if (handles.cmdbuf) dkCmdBufDestroy(static_cast<DkCmdBuf>(handles.cmdbuf));
  if (g_cmd_mem) {
    dkMemBlockDestroy(g_cmd_mem);
    g_cmd_mem = nullptr;
  }
  if (handles.device) dkDeviceDestroy(static_cast<DkDevice>(handles.device));
#endif
  handles.queue = nullptr;
  handles.cmdbuf = nullptr;
  handles.device = nullptr;
}

bool present_rgba(const std::uint32_t *pixels, std::uint32_t width, std::uint32_t height,
                  std::uint64_t frame_osd) {
  if (!pixels || width == 0 || height == 0) return false;
  const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  return gt4::platform::nx::deko3d_present_frame(reinterpret_cast<const std::uint8_t *>(pixels), bytes,
                                                 static_cast<int>(width), static_cast<int>(height), frame_osd);
}

} // namespace hwgs::dk
