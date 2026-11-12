#include "platform/nx/deko3d_present.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#include <deko3d.h>
#endif

namespace gt4::platform::nx {
namespace {
PresentConfig g_config{};
bool g_alive = false;

#if defined(__SWITCH__)
constexpr int kFbNum = 2;
constexpr int kFbW = 640;
constexpr int kFbH = 448;

bool g_fb_libnx = false;
Framebuffer g_libnx_fb{};

bool g_dk = false;
DkDevice g_device = nullptr;
DkMemBlock g_fb_mem = nullptr;
DkImage g_framebuffers[kFbNum]{};
DkSwapchain g_swapchain = nullptr;
DkMemBlock g_cmd_mem = nullptr;
DkCmdBuf g_cmdbuf = nullptr;
DkQueue g_queue = nullptr;
DkMemBlock g_stage_mem = nullptr;
DkImage g_stage_image{};
uint32_t g_stage_pitch = 0; // bytes per row
std::uint8_t *g_stage_cpu = nullptr;

void draw_osd_rgba(std::uint32_t *dst, int dst_w, int dst_h, int pitch_px, std::uint64_t frame) {
  const int bar_h = 28;
  const int bar_y = dst_h - bar_h - 40;
  const std::uint64_t span = 300;
  const int bar_w = static_cast<int>((std::min(frame, span) * static_cast<std::uint64_t>(dst_w)) / span);
  const std::uint32_t cyan = RGBA8_MAXALPHA(0, 200, 255);
  const std::uint32_t dark = RGBA8_MAXALPHA(0, 40, 60);
  for (int y = 0; y < bar_h; ++y) {
    std::uint32_t *row = dst + (bar_y + y) * pitch_px;
    for (int x = 0; x < dst_w; ++x) row[x] = (x < bar_w) ? cyan : dark;
  }
  const int ticks = static_cast<int>(std::min<std::uint64_t>(frame / 60u, static_cast<std::uint64_t>(dst_w / 8)));
  for (int t = 0; t < ticks; ++t) {
    const int x0 = 8 + t * 8;
    for (int y = 8; y < 20; ++y) {
      std::uint32_t *row = dst + y * pitch_px;
      row[x0] = RGBA8_MAXALPHA(255, 255, 255);
      row[x0 + 1] = RGBA8_MAXALPHA(255, 255, 255);
    }
  }
}

void destroy_libnx_fb() {
  if (!g_fb_libnx) return;
  framebufferClose(&g_libnx_fb);
  std::memset(&g_libnx_fb, 0, sizeof(g_libnx_fb));
  g_fb_libnx = false;
}

void destroy_deko() {
  if (!g_dk) return;
  if (g_queue) dkQueueWaitIdle(g_queue);
  if (g_queue) dkQueueDestroy(g_queue);
  if (g_cmdbuf) dkCmdBufDestroy(g_cmdbuf);
  if (g_cmd_mem) dkMemBlockDestroy(g_cmd_mem);
  if (g_stage_mem) dkMemBlockDestroy(g_stage_mem);
  if (g_swapchain) dkSwapchainDestroy(g_swapchain);
  if (g_fb_mem) dkMemBlockDestroy(g_fb_mem);
  if (g_device) dkDeviceDestroy(g_device);
  g_queue = nullptr;
  g_cmdbuf = nullptr;
  g_cmd_mem = nullptr;
  g_stage_mem = nullptr;
  g_stage_cpu = nullptr;
  g_swapchain = nullptr;
  g_fb_mem = nullptr;
  g_device = nullptr;
  g_dk = false;
}

bool begin_libnx_fb() {
  destroy_libnx_fb();
  std::memset(&g_libnx_fb, 0, sizeof(g_libnx_fb));
  const u32 w = static_cast<u32>(g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW);
  const u32 h = static_cast<u32>(g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH);
  if (R_FAILED(framebufferCreate(&g_libnx_fb, nwindowGetDefault(), w, h, PIXEL_FORMAT_RGBA_8888, 2))) return false;
  if (R_FAILED(framebufferMakeLinear(&g_libnx_fb))) {
    framebufferClose(&g_libnx_fb);
    return false;
  }
  g_fb_libnx = true;
  return true;
}

bool begin_deko() {
  destroy_deko();

  DkDeviceMaker deviceMaker;
  dkDeviceMakerDefaults(&deviceMaker);
  g_device = dkDeviceCreate(&deviceMaker);
  if (!g_device) return false;

  const int fb_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
  const int fb_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;

  DkImageLayoutMaker layoutMaker;
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine |
                      DkImageFlags_HwCompression;
  layoutMaker.format = DkImageFormat_RGBA8_Unorm;
  layoutMaker.dimensions[0] = static_cast<uint32_t>(fb_w);
  layoutMaker.dimensions[1] = static_cast<uint32_t>(fb_h);

  DkImageLayout fbLayout;
  dkImageLayoutInitialize(&fbLayout, &layoutMaker);
  uint32_t fbSize = static_cast<uint32_t>(dkImageLayoutGetSize(&fbLayout));
  uint32_t fbAlign = dkImageLayoutGetAlignment(&fbLayout);
  fbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

  DkMemBlockMaker memMaker;
  dkMemBlockMakerDefaults(&memMaker, g_device, fbSize * kFbNum);
  memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_fb_mem = dkMemBlockCreate(&memMaker);
  if (!g_fb_mem) {
    destroy_deko();
    return false;
  }

  DkImage const *swapchainImages[kFbNum];
  for (int i = 0; i < kFbNum; ++i) {
    swapchainImages[i] = &g_framebuffers[i];
    dkImageInitialize(&g_framebuffers[i], &fbLayout, g_fb_mem, static_cast<uint32_t>(i) * fbSize);
  }

  DkSwapchainMaker swapMaker;
  dkSwapchainMakerDefaults(&swapMaker, g_device, nwindowGetDefault(), swapchainImages, kFbNum);
  g_swapchain = dkSwapchainCreate(&swapMaker);
  if (!g_swapchain) {
    destroy_deko();
    return false;
  }

  // Pitch-linear staging image the CPU writes every frame; GPU blits to the swapchain.
  g_stage_pitch = (static_cast<uint32_t>(fb_w) * 4u + (DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1)) &
                  ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
  layoutMaker.format = DkImageFormat_RGBA8_Unorm;
  layoutMaker.dimensions[0] = static_cast<uint32_t>(fb_w);
  layoutMaker.dimensions[1] = static_cast<uint32_t>(fb_h);
  layoutMaker.pitchStride = g_stage_pitch;

  DkImageLayout stageLayout;
  dkImageLayoutInitialize(&stageLayout, &layoutMaker);
  const uint32_t stageSize = static_cast<uint32_t>(dkImageLayoutGetSize(&stageLayout));
  const uint32_t stageAlign = dkImageLayoutGetAlignment(&stageLayout);

  dkMemBlockMakerDefaults(&memMaker, g_device, (stageSize + stageAlign - 1) & ~(stageAlign - 1));
  memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_stage_mem = dkMemBlockCreate(&memMaker);
  if (!g_stage_mem) {
    destroy_deko();
    return false;
  }
  dkImageInitialize(&g_stage_image, &stageLayout, g_stage_mem, 0);
  g_stage_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(g_stage_mem));
  if (!g_stage_cpu) {
    destroy_deko();
    return false;
  }

  dkMemBlockMakerDefaults(&memMaker, g_device, 64 * 1024);
  memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g_cmd_mem = dkMemBlockCreate(&memMaker);
  if (!g_cmd_mem) {
    destroy_deko();
    return false;
  }

  DkCmdBufMaker cmdMaker;
  dkCmdBufMakerDefaults(&cmdMaker, g_device);
  g_cmdbuf = dkCmdBufCreate(&cmdMaker);
  if (!g_cmdbuf) {
    destroy_deko();
    return false;
  }
  dkCmdBufAddMemory(g_cmdbuf, g_cmd_mem, 0, 64 * 1024);

  DkQueueMaker queueMaker;
  dkQueueMakerDefaults(&queueMaker, g_device);
  queueMaker.flags = DkQueueFlags_Graphics;
  g_queue = dkQueueCreate(&queueMaker);
  if (!g_queue) {
    destroy_deko();
    return false;
  }

  g_dk = true;
  return true;
}

bool present_staging(std::uint64_t frame_osd) {
  if (!g_dk || !g_stage_cpu) return false;

  const int fb_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
  const int fb_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;
  const int pitch_px = static_cast<int>(g_stage_pitch / 4u);
  draw_osd_rgba(reinterpret_cast<std::uint32_t *>(g_stage_cpu), fb_w, fb_h, pitch_px, frame_osd);

  const int slot = dkQueueAcquireImage(g_queue, g_swapchain);
  dkCmdBufClear(g_cmdbuf);

  DkImageView srcView;
  dkImageViewDefaults(&srcView, &g_stage_image);
  DkImageView dstView;
  dkImageViewDefaults(&dstView, &g_framebuffers[slot]);

  DkImageRect srcRect{0, 0, 0, static_cast<uint32_t>(fb_w), static_cast<uint32_t>(fb_h), 1};
  DkImageRect dstRect{0, 0, 0, static_cast<uint32_t>(fb_w), static_cast<uint32_t>(fb_h), 1};
  dkCmdBufBlitImage(g_cmdbuf, &srcView, &srcRect, &dstView, &dstRect, DkBlitFlag_FilterNearest, 0);

  DkCmdList list = dkCmdBufFinishList(g_cmdbuf);
  dkQueueSubmitCommands(g_queue, list);
  dkQueuePresentImage(g_queue, g_swapchain, slot);
  return true;
}

bool present_libnx_rgba(const std::uint8_t *rgba, int width, int height, std::uint64_t frame_osd) {
  if (!g_fb_libnx || !rgba) return false;
  u32 stride = 0;
  auto *dst = static_cast<std::uint32_t *>(framebufferBegin(&g_libnx_fb, &stride));
  if (!dst) return false;
  const int dst_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
  const int dst_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;
  const int pitch_px = static_cast<int>(stride / 4u);
  const auto *src = reinterpret_cast<const std::uint32_t *>(rgba);

  // Exact fit: one memcpy per row (FMV / 480i menus when FB matches the guest).
  if (width == dst_w && height == dst_h && static_cast<int>(stride) == dst_w * 4) {
    std::memcpy(dst, src, static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) * 4u);
  } else if (width == dst_w && height == dst_h) {
    for (int y = 0; y < dst_h; ++y) {
      std::memcpy(reinterpret_cast<std::uint8_t *>(dst) + static_cast<std::size_t>(y) * stride,
                  src + static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                  static_cast<std::size_t>(width) * 4u);
    }
  } else {
    // Letterbox once per size change; reuse black bars.
    static int cleared_w = -1, cleared_h = -1;
    if (cleared_w != dst_w || cleared_h != dst_h) {
      for (int y = 0; y < dst_h; ++y) {
        std::memset(reinterpret_cast<std::uint8_t *>(dst) + static_cast<std::size_t>(y) * stride, 0,
                    static_cast<std::size_t>(dst_w) * 4u);
      }
      cleared_w = dst_w;
      cleared_h = dst_h;
    }
    const int dw = std::min(dst_w, width);
    const int dh = std::min(dst_h, height);
    const int ox = (dst_w - dw) / 2;
    const int oy = (dst_h - dh) / 2;
    // Center-crop or 1:1 copy of the overlapping window — no per-pixel upscale on the CPU.
    const int src_x0 = (width - dw) / 2;
    const int src_y0 = (height - dh) / 2;
    for (int y = 0; y < dh; ++y) {
      auto *row = reinterpret_cast<std::uint32_t *>(reinterpret_cast<std::uint8_t *>(dst) +
                                                    static_cast<std::size_t>(oy + y) * stride) +
                  ox;
      std::memcpy(row, src + static_cast<std::size_t>(src_y0 + y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(src_x0),
                  static_cast<std::size_t>(dw) * 4u);
    }
  }
  draw_osd_rgba(dst, dst_w, dst_h, pitch_px, frame_osd);
  framebufferEnd(&g_libnx_fb);
  return true;
}

void blit_guest_into_stage(const std::uint32_t *src, int width, int height) {
  const int fb_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
  const int fb_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;
  const int pitch_px = static_cast<int>(g_stage_pitch / 4u);
  auto *dst = reinterpret_cast<std::uint32_t *>(g_stage_cpu);
  for (int y = 0; y < fb_h; ++y) {
    std::memset(dst + y * pitch_px, 0, static_cast<std::size_t>(fb_w) * 4u);
  }
  const float scale = std::min(static_cast<float>(fb_w) / static_cast<float>(width),
                               static_cast<float>(fb_h) / static_cast<float>(height));
  const int dw = std::max(1, static_cast<int>(static_cast<float>(width) * scale));
  const int dh = std::max(1, static_cast<int>(static_cast<float>(height) * scale));
  const int ox = (fb_w - dw) / 2;
  const int oy = (fb_h - dh) / 2;
  for (int y = 0; y < dh; ++y) {
    const int sy = y * height / dh;
    std::uint32_t *row = dst + (oy + y) * pitch_px + ox;
    for (int x = 0; x < dw; ++x) {
      const int sx = x * width / dw;
      row[x] = src[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(sx)];
    }
  }
}
#endif

} // namespace

bool deko3d_present_create(const PresentConfig &config) {
  g_config = config;
  g_alive = true;
  return true;
}

void deko3d_present_destroy() {
  soft_present_end();
  g_alive = false;
}

bool soft_present_begin() {
#if defined(__SWITCH__)
  // Prefer the proven libnx linear FB. The first deko3d blit path scrambled guest RGBA
  // (pitch/format); keep begin_deko() for the next CoreGS GPU present attempt only.
  if (begin_libnx_fb()) {
    g_alive = true;
    return true;
  }
  if (begin_deko()) {
    g_alive = true;
    return true;
  }
  return false;
#else
  return g_alive;
#endif
}

void soft_present_end() {
#if defined(__SWITCH__)
  destroy_deko();
  destroy_libnx_fb();
#endif
}

bool deko3d_present_frame(const std::uint8_t *rgba, std::size_t bytes, int width, int height,
                          std::uint64_t frame_osd) {
  if (!rgba || width <= 0 || height <= 0) return false;
  const std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (bytes < need) return false;
#if defined(__SWITCH__)
  g_alive = true;
  if (g_dk) {
    blit_guest_into_stage(reinterpret_cast<const std::uint32_t *>(rgba), width, height);
    return present_staging(frame_osd);
  }
  if (g_fb_libnx) return present_libnx_rgba(rgba, width, height, frame_osd);
  return false;
#else
  (void)frame_osd;
  (void)g_config;
  return true;
#endif
}

bool soft_present_heartbeat(std::uint64_t frame) {
#if defined(__SWITCH__)
  if (g_dk) {
    const int fb_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
    const int fb_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;
    const int pitch_px = static_cast<int>(g_stage_pitch / 4u);
    auto *dst = reinterpret_cast<std::uint32_t *>(g_stage_cpu);
    for (int y = 0; y < fb_h; ++y) std::memset(dst + y * pitch_px, 0, static_cast<std::size_t>(fb_w) * 4u);
    return present_staging(frame);
  }
  if (g_fb_libnx) {
    // Black frame + OSD through the libnx path.
    static std::vector<std::uint32_t> black;
    black.assign(16 * 16, 0u);
    return present_libnx_rgba(reinterpret_cast<const std::uint8_t *>(black.data()), 16, 16, frame);
  }
  return false;
#else
  (void)frame;
  return false;
#endif
}

} // namespace gt4::platform::nx
