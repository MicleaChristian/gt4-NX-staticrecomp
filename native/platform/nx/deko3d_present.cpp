#include "platform/nx/deko3d_present.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#include <deko3d.h>

#include "perf_nx.hpp"
#endif

namespace gt4::platform::nx {
namespace {
PresentConfig g_config{};
bool g_alive = false;

#if defined(__SWITCH__)
constexpr int kFbNum = 2;
constexpr int kStageSlots = 2;
constexpr int kFbW = 1280;
constexpr int kFbH = 720;
constexpr int kGuestMaxW = 1024;
constexpr int kGuestMaxH = 1024;

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

// Double-buffered guest staging (tight RGBA8). CPU writes slot N only after its fence.
DkMemBlock g_stage_mem = nullptr;
std::uint32_t *g_stage_cpu[kStageSlots] = {};
DkFence g_stage_fence[kStageSlots]{};
bool g_stage_fence_live[kStageSlots] = {};
int g_stage_write = 0;
uint32_t g_stage_slot_bytes = 0;

// Pitch-linear guest image the 2D engine blits from (GPU scale / letterbox).
DkMemBlock g_guest_mem = nullptr;
DkImage g_guest_image{};
DkImageLayout g_guest_layout{};
uint32_t g_guest_pitch = 0;
int g_guest_w = 0;
int g_guest_h = 0;

int g_last_present_w = 0;
int g_last_present_h = 0;
bool g_have_last_present = false;

int fb_width() { return g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW; }
int fb_height() { return g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH; }

void letterbox_rects(int src_w, int src_h, int dst_w, int dst_h, int &ox, int &oy, int &dw, int &dh) {
  const float scale = std::min(static_cast<float>(dst_w) / static_cast<float>(src_w),
                               static_cast<float>(dst_h) / static_cast<float>(src_h));
  dw = std::max(1, static_cast<int>(static_cast<float>(src_w) * scale));
  dh = std::max(1, static_cast<int>(static_cast<float>(src_h) * scale));
  ox = (dst_w - dw) / 2;
  oy = (dst_h - dh) / 2;
}

void destroy_libnx_fb() {
  if (!g_fb_libnx) return;
  framebufferClose(&g_libnx_fb);
  std::memset(&g_libnx_fb, 0, sizeof(g_libnx_fb));
  g_fb_libnx = false;
}

void destroy_deko() {
  if (!g_dk && !g_device) return;
  if (g_queue) dkQueueWaitIdle(g_queue);
  if (g_queue) dkQueueDestroy(g_queue);
  if (g_cmdbuf) dkCmdBufDestroy(g_cmdbuf);
  if (g_cmd_mem) dkMemBlockDestroy(g_cmd_mem);
  if (g_stage_mem) dkMemBlockDestroy(g_stage_mem);
  if (g_guest_mem) dkMemBlockDestroy(g_guest_mem);
  if (g_swapchain) dkSwapchainDestroy(g_swapchain);
  if (g_fb_mem) dkMemBlockDestroy(g_fb_mem);
  if (g_device) dkDeviceDestroy(g_device);
  g_queue = nullptr;
  g_cmdbuf = nullptr;
  g_cmd_mem = nullptr;
  g_stage_mem = nullptr;
  g_guest_mem = nullptr;
  for (int i = 0; i < kStageSlots; ++i) {
    g_stage_cpu[i] = nullptr;
    g_stage_fence_live[i] = false;
  }
  g_swapchain = nullptr;
  g_fb_mem = nullptr;
  g_device = nullptr;
  g_guest_w = g_guest_h = 0;
  g_have_last_present = false;
  g_dk = false;
}

bool begin_libnx_fb() {
  destroy_libnx_fb();
  std::memset(&g_libnx_fb, 0, sizeof(g_libnx_fb));
  const u32 w = static_cast<u32>(fb_width());
  const u32 h = static_cast<u32>(fb_height());
  if (R_FAILED(framebufferCreate(&g_libnx_fb, nwindowGetDefault(), w, h, PIXEL_FORMAT_RGBA_8888, 2))) return false;
  if (R_FAILED(framebufferMakeLinear(&g_libnx_fb))) {
    framebufferClose(&g_libnx_fb);
    return false;
  }
  g_fb_libnx = true;
  return true;
}

bool ensure_guest_image(int width, int height) {
  if (width <= 0 || height <= 0 || width > kGuestMaxW || height > kGuestMaxH) return false;
  if (g_guest_mem && g_guest_w == width && g_guest_h == height) return true;

  // Layout/image can be re-initialised over the same max-sized mem block.
  if (!g_guest_mem) return false;
  g_guest_pitch = (static_cast<uint32_t>(width) * 4u + (DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1)) &
                  ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
  DkImageLayoutMaker layoutMaker;
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
  layoutMaker.format = DkImageFormat_RGBA8_Unorm;
  layoutMaker.dimensions[0] = static_cast<uint32_t>(width);
  layoutMaker.dimensions[1] = static_cast<uint32_t>(height);
  layoutMaker.pitchStride = g_guest_pitch;
  dkImageLayoutInitialize(&g_guest_layout, &layoutMaker);
  dkImageInitialize(&g_guest_image, &g_guest_layout, g_guest_mem, 0);
  g_guest_w = width;
  g_guest_h = height;
  return true;
}

bool begin_deko() {
  destroy_deko();
  const int fb_w = fb_width();
  const int fb_h = fb_height();

  DkDeviceMaker deviceMaker;
  dkDeviceMakerDefaults(&deviceMaker);
  g_device = dkDeviceCreate(&deviceMaker);
  if (!g_device) return false;

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
  const uint32_t fbAlign = dkImageLayoutGetAlignment(&fbLayout);
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

  g_stage_slot_bytes = static_cast<uint32_t>(kGuestMaxW) * static_cast<uint32_t>(kGuestMaxH) * 4u;
  const uint32_t stage_total = ((g_stage_slot_bytes * kStageSlots) + 0xFFFu) & ~0xFFFu;
  dkMemBlockMakerDefaults(&memMaker, g_device, stage_total);
  memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g_stage_mem = dkMemBlockCreate(&memMaker);
  if (!g_stage_mem) {
    destroy_deko();
    return false;
  }
  auto *base = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(g_stage_mem));
  for (int i = 0; i < kStageSlots; ++i) {
    g_stage_cpu[i] = reinterpret_cast<std::uint32_t *>(base + static_cast<std::size_t>(i) * g_stage_slot_bytes);
    std::memset(g_stage_cpu[i], 0, g_stage_slot_bytes);
    g_stage_fence_live[i] = false;
  }
  g_stage_write = 0;

  // Max-sized pitch-linear guest target for GPU blit.
  const uint32_t guest_pitch =
      (static_cast<uint32_t>(kGuestMaxW) * 4u + (DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1)) &
      ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
  layoutMaker.format = DkImageFormat_RGBA8_Unorm;
  layoutMaker.dimensions[0] = kGuestMaxW;
  layoutMaker.dimensions[1] = kGuestMaxH;
  layoutMaker.pitchStride = guest_pitch;
  DkImageLayout maxGuestLayout;
  dkImageLayoutInitialize(&maxGuestLayout, &layoutMaker);
  uint32_t guest_bytes = static_cast<uint32_t>(dkImageLayoutGetSize(&maxGuestLayout));
  const uint32_t guest_align = dkImageLayoutGetAlignment(&maxGuestLayout);
  guest_bytes = (guest_bytes + guest_align - 1) & ~(guest_align - 1);
  dkMemBlockMakerDefaults(&memMaker, g_device, guest_bytes);
  memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_guest_mem = dkMemBlockCreate(&memMaker);
  if (!g_guest_mem) {
    destroy_deko();
    return false;
  }

  dkMemBlockMakerDefaults(&memMaker, g_device, 256 * 1024);
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
  dkCmdBufAddMemory(g_cmdbuf, g_cmd_mem, 0, 256 * 1024);

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

bool present_gpu_scaled(const std::uint32_t *src, int width, int height) {
  if (!g_dk || !g_stage_cpu[0]) return false;
  if (width > kGuestMaxW || height > kGuestMaxH) return false;
  if (!ensure_guest_image(width, height)) return false;

  const int slot = g_stage_write;
  if (g_stage_fence_live[slot]) {
    dkFenceWait(&g_stage_fence[slot], -1);
    g_stage_fence_live[slot] = false;
  }

  // Tight packed guest → staging slot (no 720p letterbox on CPU).
  const std::size_t guest_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  std::memcpy(g_stage_cpu[slot], src, guest_bytes);

  const int fb_w = fb_width();
  const int fb_h = fb_height();
  int ox = 0, oy = 0, dw = fb_w, dh = fb_h;
  letterbox_rects(width, height, fb_w, fb_h, ox, oy, dw, dh);

  const int swap = dkQueueAcquireImage(g_queue, g_swapchain);
  dkCmdBufClear(g_cmdbuf);

  // Clear swapchain to black, then blit guest into the letterboxed rect.
  DkImageView dstView;
  dkImageViewDefaults(&dstView, &g_framebuffers[swap]);
  dkCmdBufBindRenderTarget(g_cmdbuf, &dstView, nullptr);
  DkViewport vp{0, 0, static_cast<float>(fb_w), static_cast<float>(fb_h), 0, 1};
  DkScissor sc{0, 0, static_cast<uint32_t>(fb_w), static_cast<uint32_t>(fb_h)};
  dkCmdBufSetViewports(g_cmdbuf, 0, &vp, 1);
  dkCmdBufSetScissors(g_cmdbuf, 0, &sc, 1);
  dkCmdBufClearColorFloat(g_cmdbuf, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);

  const DkCopyBuf linearSrc{dkMemBlockGetGpuAddr(g_stage_mem) +
                                static_cast<DkGpuAddr>(slot) * g_stage_slot_bytes,
                            0, 0};
  DkImageView guestView;
  dkImageViewDefaults(&guestView, &g_guest_image);
  const DkImageRect guestRect{0, 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  dkCmdBufCopyBufferToImage(g_cmdbuf, &linearSrc, &guestView, &guestRect, 0);

  const DkImageRect srcRect{0, 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  const DkImageRect dstRect{static_cast<uint32_t>(ox), static_cast<uint32_t>(oy), 0, static_cast<uint32_t>(dw),
                            static_cast<uint32_t>(dh), 1};
  dkCmdBufBlitImage(g_cmdbuf, &guestView, &srcRect, &dstView, &dstRect, DkBlitFlag_FilterLinear, 0);

  dkCmdBufSignalFence(g_cmdbuf, &g_stage_fence[slot], true);
  DkCmdList list = dkCmdBufFinishList(g_cmdbuf);
  dkQueueSubmitCommands(g_queue, list);
  dkQueuePresentImage(g_queue, g_swapchain, swap);
  g_stage_fence_live[slot] = true;
  g_stage_write = (slot + 1) % kStageSlots;
  g_last_present_w = width;
  g_last_present_h = height;
  g_have_last_present = true;
  return true;
}

bool present_libnx_rgba(const std::uint8_t *rgba, int width, int height) {
  if (!g_fb_libnx || !rgba) return false;
  u32 stride = 0;
  auto *dst = static_cast<std::uint32_t *>(framebufferBegin(&g_libnx_fb, &stride));
  if (!dst) return false;
  const int dst_w = fb_width();
  const int dst_h = fb_height();
  int ox = 0, oy = 0, dw = 0, dh = 0;
  letterbox_rects(width, height, dst_w, dst_h, ox, oy, dw, dh);
  const int pitch_px = static_cast<int>(stride / 4u);
  for (int y = 0; y < dst_h; ++y) {
    std::memset(dst + static_cast<std::size_t>(y) * pitch_px, 0, static_cast<std::size_t>(dst_w) * 4u);
  }
  const auto *src = reinterpret_cast<const std::uint32_t *>(rgba);
  for (int y = 0; y < dh; ++y) {
    const int sy = y * height / dh;
    auto *row = dst + static_cast<std::size_t>(oy + y) * pitch_px + ox;
    for (int x = 0; x < dw; ++x) {
      const int sx = x * width / dw;
      row[x] = src[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(sx)];
    }
  }
  framebufferEnd(&g_libnx_fb);
  return true;
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
  if (begin_deko()) {
    g_alive = true;
    return true;
  }
  if (begin_libnx_fb()) {
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
  (void)frame_osd;
  if (!rgba || width <= 0 || height <= 0) return false;
  const std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (bytes < need) return false;
#if defined(__SWITCH__)
  gt4::perf::note_present_size(width, height);
  gt4::perf::Scope scope(gt4::perf::kPresent);
  g_alive = true;
  if (g_dk) {
    return present_gpu_scaled(reinterpret_cast<const std::uint32_t *>(rgba), width, height);
  }
  if (g_fb_libnx) return present_libnx_rgba(rgba, width, height);
  return false;
#else
  (void)g_config;
  return true;
#endif
}

bool soft_present_heartbeat(std::uint64_t frame) {
  (void)frame;
#if defined(__SWITCH__)
  // Re-present last guest size as black only when we have never shown a frame.
  if (g_have_last_present && g_dk) {
    static std::vector<std::uint32_t> black;
    const int w = std::max(1, g_last_present_w);
    const int h = std::max(1, g_last_present_h);
    black.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
    return deko3d_present_frame(reinterpret_cast<const std::uint8_t *>(black.data()), black.size() * 4u, w, h, 0);
  }
  static const std::uint32_t kBlack[16 * 16] = {};
  return deko3d_present_frame(reinterpret_cast<const std::uint8_t *>(kBlack), sizeof kBlack, 16, 16, 0);
#else
  return false;
#endif
}

} // namespace gt4::platform::nx
