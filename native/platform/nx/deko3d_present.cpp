#include <atomic>
#include <cstdio>
#include "platform/nx/deko3d_present.hpp"

#include "perf_nx.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__has_include)
#  if __has_include("gt4_nx_build_id.hpp")
#    include "gt4_nx_build_id.hpp" // team.py: static constexpr char kGt4NxBuildId[]
#    define GT4_NX_BUILD_ID_FROM_HEADER 1
#  endif
#endif
#if !defined(GT4_NX_BUILD_ID_FROM_HEADER)
#  ifndef GT4_NX_BUILD_ID
#    define GT4_NX_BUILD_ID "local"
#  endif
static constexpr char kGt4NxBuildId[] = GT4_NX_BUILD_ID;
#endif

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

// Host-only build/candidate ID OSD (NX-2026-11-13-N). Opaque dark bar + cyan 5x7 glyphs.
// RGBA8 LE: R in low byte (same packing as pack_rgba / prior magenta probe).
constexpr std::uint32_t kStampBg = 0xFF101010u;
constexpr std::uint32_t kStampFg = 0xFFFFFF00u; // cyan

// 5x7 bitmaps: bit0 = top row … bit6 = bottom; five columns left→right.
struct StampGlyph {
  char ch;
  std::uint8_t cols[5];
};

constexpr StampGlyph kStampGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x00, 0x40, 0x00, 0x00}},
    {'|', {0x00, 0x00, 0x7F, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
    {'b', {0x7F, 0x48, 0x44, 0x44, 0x38}},
    {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
    {'d', {0x38, 0x44, 0x44, 0x48, 0x7F}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
    {'f', {0x08, 0x7E, 0x09, 0x01, 0x02}},
    {'g', {0x08, 0x14, 0x54, 0x54, 0x3C}},
    {'h', {0x7F, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7D, 0x40, 0x00}},
    {'j', {0x20, 0x40, 0x44, 0x3D, 0x00}},
    {'k', {0x7F, 0x10, 0x28, 0x44, 0x00}},
    {'l', {0x00, 0x41, 0x7F, 0x40, 0x00}},
    {'m', {0x7C, 0x04, 0x18, 0x04, 0x78}},
    {'n', {0x7C, 0x08, 0x04, 0x04, 0x78}},
    {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
    {'p', {0x7C, 0x14, 0x14, 0x14, 0x08}},
    {'q', {0x08, 0x14, 0x14, 0x18, 0x7C}},
    {'r', {0x7C, 0x08, 0x04, 0x04, 0x08}},
    {'s', {0x48, 0x54, 0x54, 0x54, 0x24}},
    {'t', {0x04, 0x3F, 0x44, 0x40, 0x20}},
    {'u', {0x3C, 0x40, 0x40, 0x20, 0x7C}},
    {'v', {0x1C, 0x20, 0x40, 0x20, 0x1C}},
    {'w', {0x3C, 0x40, 0x30, 0x40, 0x3C}},
    {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
    {'y', {0x0C, 0x50, 0x50, 0x50, 0x3C}},
    {'z', {0x44, 0x64, 0x54, 0x4C, 0x44}},
};

const StampGlyph *find_stamp_glyph(char ch) {
  for (const StampGlyph &g : kStampGlyphs) {
    if (g.ch == ch) return &g;
  }
  return nullptr;
}

void stamp_text(std::uint32_t *dst, int pitch_px, int w, int h, const char *text);
void stamp_build_id(std::uint32_t *dst, int pitch_px, int w, int h) { stamp_text(dst, pitch_px, w, h, kGt4NxBuildId); }

void stamp_text(std::uint32_t *dst, int pitch_px, int w, int h, const char *text) {
  if (!dst || pitch_px <= 0 || w < 32 || h < 12) return;
  int len = 0;
  while (text[len] != '\0' && len < 64) ++len;
  if (len == 0) return;

  constexpr int kPadX = 2;
  constexpr int kPadY = 2;
  constexpr int kGlyphW = 5;
  constexpr int kGlyphH = 7;
  constexpr int kGap = 1;
  const int bar_w = std::min(w, kPadX * 2 + len * (kGlyphW + kGap));
  const int bar_h = std::min(h, kPadY * 2 + kGlyphH);
  // Top-left so Album crops and menu chrome still show the ID.
  for (int y = 0; y < bar_h; ++y) {
    std::uint32_t *row = dst + y * pitch_px;
    for (int x = 0; x < bar_w; ++x) row[x] = kStampBg;
  }
  for (int i = 0; i < len; ++i) {
    const StampGlyph *g = find_stamp_glyph(text[i]);
    if (!g) g = find_stamp_glyph(' ');
    if (!g) continue;
    const int ox = kPadX + i * (kGlyphW + kGap);
    const int oy = kPadY;
    for (int col = 0; col < kGlyphW; ++col) {
      const std::uint8_t bits = g->cols[col];
      for (int row = 0; row < kGlyphH; ++row) {
        if ((bits & (1u << row)) == 0) continue;
        const int px = ox + col;
        const int py = oy + row;
        if (px >= 0 && px < w && py >= 0 && py < h) {
          dst[py * pitch_px + px] = kStampFg;
        }
      }
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
  (void)frame_osd;
  // Stamp after guest blit / black clear so deko fallback + heartbeat show the ID.
  stamp_build_id(reinterpret_cast<std::uint32_t *>(g_stage_cpu), pitch_px, fb_w, fb_h);

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
  (void)frame_osd;
  stamp_build_id(dst, pitch_px, dst_w, dst_h);
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

// ---- Shared-device GPU present (zero-copy scanout) ----
// Design: the CoreGS renderer's device/queue own the swapchain. A scanout is one 2D-engine
// blit from the renderer's colour surface into the acquired swapchain image (stretch + line
// double in one step), plus a stamp blit. No GPU->CPU readback, no WaitIdle, no CPU memcpy.
// Two fenced command slots: a slot's memory is only rewound after its fence has signalled.
constexpr int kGpuFbNum = 3;
constexpr std::uint32_t kGpuStageW = 1024;
constexpr std::uint32_t kGpuStageH = 1024;
constexpr std::uint32_t kGpuStampW = 512;
constexpr std::uint32_t kGpuStampH = 16;
constexpr int kGpuSlots = 2;
constexpr std::uint32_t kGpuCmdSlotBytes = 16 * 1024;

struct GpuPresentSlot {
  DkCmdBuf cmdbuf = nullptr;
  DkFence fence{};
  bool pending = false;
};

bool g_gpu = false;
DkDevice g_gpu_device = nullptr;
DkQueue g_gpu_queue = nullptr;
DkMemBlock g_gpu_fb_mem = nullptr;
DkImage g_gpu_fb[kGpuFbNum]{};
DkSwapchain g_gpu_swapchain = nullptr;
DkMemBlock g_gpu_stage_mem = nullptr;
DkImage g_gpu_stage{};
std::uint32_t g_gpu_stage_pitch = 0;
std::uint8_t *g_gpu_stage_cpu = nullptr;
DkMemBlock g_gpu_stamp_mem = nullptr;
DkImage g_gpu_stamp{};
std::uint32_t g_gpu_stamp_w = 0;
std::uint32_t g_gpu_stamp_h = 0;
// NX-2026-11-27-BF: status text (frame rate) appended to the build ID.
std::uint8_t *g_gpu_stamp_cpu = nullptr;
std::uint32_t g_gpu_stamp_pitch = 0;
char g_status_text[32] = "";
std::atomic<std::uint32_t> g_status_version{0};
std::uint32_t g_status_drawn = 0;
DkMemBlock g_gpu_cmd_mem = nullptr;
GpuPresentSlot g_gpu_slots[kGpuSlots]{};
int g_gpu_next = 0;

bool make_linear_image(DkDevice dev, std::uint32_t w, std::uint32_t h, DkMemBlock &mem, DkImage &img,
                       std::uint32_t &pitch, std::uint8_t *&cpu) {
  pitch = (w * 4u + (DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1)) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, dev);
  lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = w;
  lm.dimensions[1] = h;
  lm.pitchStride = pitch;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &lm);
  const std::uint32_t size = static_cast<std::uint32_t>(dkImageLayoutGetSize(&layout));
  const std::uint32_t align = dkImageLayoutGetAlignment(&layout);
  const std::uint32_t block_align = DK_MEMBLOCK_ALIGNMENT > align ? DK_MEMBLOCK_ALIGNMENT : align;
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dev, (size + block_align - 1) & ~(block_align - 1));
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  mem = dkMemBlockCreate(&mm);
  if (!mem) return false;
  dkImageInitialize(&img, &layout, mem, 0);
  cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(mem));
  return cpu != nullptr;
}

void stamp_extent(std::uint32_t &w, std::uint32_t &h) {
  int len = 0;
  while (kGt4NxBuildId[len] != '\0' && len < 64) ++len;
  w = static_cast<std::uint32_t>(std::min<int>(static_cast<int>(kGpuStampW), 4 + len * 6));
  h = 11u;
}

void gpu_wait_all() {
  for (GpuPresentSlot &s : g_gpu_slots) {
    if (s.pending) {
      dkFenceWait(&s.fence, -1);
      s.pending = false;
    }
  }
}

void destroy_gpu() {
  if (!g_gpu && !g_gpu_device) return;
  if (g_gpu_queue) dkQueueWaitIdle(g_gpu_queue);
  for (GpuPresentSlot &s : g_gpu_slots) {
    if (s.cmdbuf) dkCmdBufDestroy(s.cmdbuf);
    s = GpuPresentSlot{};
  }
  if (g_gpu_cmd_mem) dkMemBlockDestroy(g_gpu_cmd_mem);
  if (g_gpu_stamp_mem) dkMemBlockDestroy(g_gpu_stamp_mem);
  if (g_gpu_stage_mem) dkMemBlockDestroy(g_gpu_stage_mem);
  if (g_gpu_swapchain) dkSwapchainDestroy(g_gpu_swapchain);
  if (g_gpu_fb_mem) dkMemBlockDestroy(g_gpu_fb_mem);
  g_gpu_cmd_mem = nullptr;
  g_gpu_stamp_mem = nullptr;
  g_gpu_stage_mem = nullptr;
  g_gpu_stage_cpu = nullptr;
  g_gpu_swapchain = nullptr;
  g_gpu_fb_mem = nullptr;
  g_gpu_queue = nullptr;  // owned by the renderer
  g_gpu_device = nullptr; // owned by the renderer
  g_gpu_next = 0;
  g_gpu = false;
}

bool begin_gpu(DkDevice dev, DkQueue queue) {
  destroy_gpu();
  g_gpu_device = dev;
  g_gpu_queue = queue;
  const std::uint32_t fb_w =
      static_cast<std::uint32_t>(g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW);
  const std::uint32_t fb_h =
      static_cast<std::uint32_t>(g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH);

  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, dev);
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = fb_w;
  lm.dimensions[1] = fb_h;
  DkImageLayout fbLayout;
  dkImageLayoutInitialize(&fbLayout, &lm);
  const std::uint32_t fbAlign = dkImageLayoutGetAlignment(&fbLayout);
  const std::uint32_t fbSize =
      (static_cast<std::uint32_t>(dkImageLayoutGetSize(&fbLayout)) + fbAlign - 1) & ~(fbAlign - 1);
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dev, (fbSize * kGpuFbNum + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  g_gpu_fb_mem = dkMemBlockCreate(&mm);
  if (!g_gpu_fb_mem) {
    destroy_gpu();
    return false;
  }
  DkImage const *images[kGpuFbNum];
  for (int i = 0; i < kGpuFbNum; ++i) {
    dkImageInitialize(&g_gpu_fb[i], &fbLayout, g_gpu_fb_mem, static_cast<std::uint32_t>(i) * fbSize);
    images[i] = &g_gpu_fb[i];
  }
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, dev, nwindowGetDefault(), images, kGpuFbNum);
  g_gpu_swapchain = dkSwapchainCreate(&sm);
  if (!g_gpu_swapchain) {
    destroy_gpu();
    return false;
  }

  if (!make_linear_image(dev, kGpuStageW, kGpuStageH, g_gpu_stage_mem, g_gpu_stage, g_gpu_stage_pitch,
                         g_gpu_stage_cpu)) {
    destroy_gpu();
    return false;
  }

  std::uint32_t stamp_pitch = 0;
  std::uint8_t *stamp_cpu = nullptr;
  if (!make_linear_image(dev, kGpuStampW, kGpuStampH, g_gpu_stamp_mem, g_gpu_stamp, stamp_pitch, stamp_cpu)) {
    destroy_gpu();
    return false;
  }
  // Filled once: the build ID is compile-time.
  std::memset(stamp_cpu, 0, static_cast<std::size_t>(stamp_pitch) * kGpuStampH);
  stamp_build_id(reinterpret_cast<std::uint32_t *>(stamp_cpu), static_cast<int>(stamp_pitch / 4u),
                 static_cast<int>(kGpuStampW), static_cast<int>(kGpuStampH));
  stamp_extent(g_gpu_stamp_w, g_gpu_stamp_h);
  g_gpu_stamp_cpu = stamp_cpu;
  g_gpu_stamp_pitch = stamp_pitch;

  dkMemBlockMakerDefaults(&mm, dev, kGpuCmdSlotBytes * kGpuSlots);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g_gpu_cmd_mem = dkMemBlockCreate(&mm);
  if (!g_gpu_cmd_mem) {
    destroy_gpu();
    return false;
  }
  for (int i = 0; i < kGpuSlots; ++i) {
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, dev);
    g_gpu_slots[i].cmdbuf = dkCmdBufCreate(&cm);
    if (!g_gpu_slots[i].cmdbuf) {
      destroy_gpu();
      return false;
    }
    dkCmdBufAddMemory(g_gpu_slots[i].cmdbuf, g_gpu_cmd_mem, static_cast<std::uint32_t>(i) * kGpuCmdSlotBytes,
                      kGpuCmdSlotBytes);
  }
  g_gpu = true;
  return true;
}

GpuPresentSlot &gpu_take_slot() {
  GpuPresentSlot &s = g_gpu_slots[g_gpu_next];
  g_gpu_next = (g_gpu_next + 1) % kGpuSlots;
  if (s.pending) {
    dkFenceWait(&s.fence, -1);
    s.pending = false;
  }
  dkCmdBufClear(s.cmdbuf);
  return s;
}

// Blit src rect -> whole swapchain image, stamp, fence, present. Queue order puts it after
// every renderer list already submitted on the same queue.
bool gpu_present_rect(const DkImage *src, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  if (!g_gpu || !src || w == 0 || h == 0) return false;
  if (dkQueueIsInErrorState(g_gpu_queue)) return false;
  const std::uint32_t fb_w =
      static_cast<std::uint32_t>(g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW);
  const std::uint32_t fb_h =
      static_cast<std::uint32_t>(g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH);

  GpuPresentSlot &s = gpu_take_slot();
  const int slot = dkQueueAcquireImage(g_gpu_queue, g_gpu_swapchain);
  dkCmdBufBarrier(s.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);

  DkImageView srcView;
  dkImageViewDefaults(&srcView, src);
  DkImageView dstView;
  dkImageViewDefaults(&dstView, &g_gpu_fb[slot]);
  const DkImageRect srcRect{x, y, 0, w, h, 1};
  const DkImageRect dstRect{0, 0, 0, fb_w, fb_h, 1};
  const bool exact = (w == fb_w && h == fb_h);
  dkCmdBufBlitImage(s.cmdbuf, &srcView, &srcRect, &dstView, &dstRect,
                    exact ? DkBlitFlag_FilterNearest : DkBlitFlag_FilterLinear, 0);

  // NX-2026-11-27-BF: redraw the stamp when the status text changed (build ID, two spaces, text)
  // and show it at twice its size so the numbers can be read on the handheld screen.
  const std::uint32_t status_version = g_status_version.load();
  if (g_gpu_stamp_cpu && status_version != g_status_drawn) {
    g_status_drawn = status_version;
    char line[96];
    std::snprintf(line, sizeof line, "%s  %.31s", kGt4NxBuildId, g_status_text);
    std::memset(g_gpu_stamp_cpu, 0, static_cast<std::size_t>(g_gpu_stamp_pitch) * kGpuStampH);
    stamp_text(reinterpret_cast<std::uint32_t *>(g_gpu_stamp_cpu), static_cast<int>(g_gpu_stamp_pitch / 4u),
               static_cast<int>(kGpuStampW), static_cast<int>(kGpuStampH), line);
    int len = 0;
    while (line[len] != '\0' && len < 64) ++len;
    g_gpu_stamp_w = static_cast<std::uint32_t>(std::min<int>(static_cast<int>(kGpuStampW), 4 + len * 6));
  }
  if (g_gpu_stamp_w > 0 && g_gpu_stamp_w * 2u <= fb_w && g_gpu_stamp_h * 2u <= fb_h) {
    DkImageView stampView;
    dkImageViewDefaults(&stampView, &g_gpu_stamp);
    const DkImageRect stampRect{0, 0, 0, g_gpu_stamp_w, g_gpu_stamp_h, 1};
    const DkImageRect stampDst{0, 0, 0, g_gpu_stamp_w * 2u, g_gpu_stamp_h * 2u, 1};
    dkCmdBufBlitImage(s.cmdbuf, &stampView, &stampRect, &dstView, &stampDst, DkBlitFlag_FilterNearest, 0);
  }

  dkCmdBufSignalFence(s.cmdbuf, &s.fence, true);
  const DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
  dkQueueSubmitCommands(g_gpu_queue, list);
  s.pending = true;
  dkQueuePresentImage(g_gpu_queue, g_gpu_swapchain, slot);
  return true;
}

// CPU frame (FMV / CPU compose fallback) through the shared swapchain: raw row copy into the
// stage, GPU does the scale. Waits both slots so the stage is not overwritten mid-blit.
bool gpu_present_cpu(const std::uint32_t *src, int width, int height) {
  if (!g_gpu || !g_gpu_stage_cpu || !src) return false;
  const std::uint32_t w = static_cast<std::uint32_t>(std::min<int>(width, static_cast<int>(kGpuStageW)));
  const std::uint32_t h = static_cast<std::uint32_t>(std::min<int>(height, static_cast<int>(kGpuStageH)));
  if (w == 0 || h == 0) return false;
  gpu_wait_all();
  for (std::uint32_t row = 0; row < h; ++row) {
    std::memcpy(g_gpu_stage_cpu + static_cast<std::size_t>(row) * g_gpu_stage_pitch,
                src + static_cast<std::size_t>(row) * static_cast<std::size_t>(width), static_cast<std::size_t>(w) * 4u);
  }
  return gpu_present_rect(&g_gpu_stage, 0, 0, w, h);
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
  if (g_gpu) { // renderer already attached its device: the deko swapchain owns the window
    g_alive = true;
    return true;
  }
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
  destroy_gpu();
  destroy_deko();
  destroy_libnx_fb();
#endif
}

bool deko3d_present_frame(const std::uint8_t *rgba, std::size_t bytes, int width, int height,
                          std::uint64_t frame_osd) {
  if (!rgba || width <= 0 || height <= 0) return false;
  const std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (bytes < need) return false;
  // Wire present_ms / present_w/h: prior CoreGS path never called Scope(kPresent) or note_present_size.
  gt4::perf::Scope present_scope(gt4::perf::kPresent);
  gt4::perf::note_present_size(width, height);
#if defined(__SWITCH__)
  g_alive = true;
  if (g_gpu) return gpu_present_cpu(reinterpret_cast<const std::uint32_t *>(rgba), width, height);
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
  gt4::perf::Scope present_scope(gt4::perf::kPresent);
#if defined(__SWITCH__)
  if (g_gpu) {
    static std::vector<std::uint32_t> black_gpu(16 * 16, 0u);
    gt4::perf::note_present_size(16, 16);
    return gpu_present_cpu(black_gpu.data(), 16, 16);
  }
  if (g_dk) {
    const int fb_w = g_config.framebuffer_width > 0 ? g_config.framebuffer_width : kFbW;
    const int fb_h = g_config.framebuffer_height > 0 ? g_config.framebuffer_height : kFbH;
    gt4::perf::note_present_size(fb_w, fb_h);
    const int pitch_px = static_cast<int>(g_stage_pitch / 4u);
    auto *dst = reinterpret_cast<std::uint32_t *>(g_stage_cpu);
    for (int y = 0; y < fb_h; ++y) std::memset(dst + y * pitch_px, 0, static_cast<std::size_t>(fb_w) * 4u);
    return present_staging(frame);
  }
  if (g_fb_libnx) {
    // Black frame + host build-ID stamp through the libnx path.
    static std::vector<std::uint32_t> black;
    black.assign(16 * 16, 0u);
    gt4::perf::note_present_size(16, 16);
    return present_libnx_rgba(reinterpret_cast<const std::uint8_t *>(black.data()), 16, 16, frame);
  }
  return false;
#else
  (void)frame;
  return false;
#endif
}

bool deko3d_present_attach_gpu(void *dk_device, void *dk_queue) {
#if defined(__SWITCH__)
  if (!dk_device || !dk_queue) return false;
  // The libnx framebuffer and our private deko swapchain both own the default window:
  // release them before the shared-device swapchain takes it.
  destroy_deko();
  destroy_libnx_fb();
  if (begin_gpu(static_cast<DkDevice>(dk_device), static_cast<DkQueue>(dk_queue))) {
    g_alive = true;
    return true;
  }
  begin_libnx_fb(); // keep a working CPU present path
  return false;
#else
  (void)dk_device;
  (void)dk_queue;
  return false;
#endif
}

void deko3d_present_detach_gpu() {
#if defined(__SWITCH__)
  if (!g_gpu) return;
  destroy_gpu();
  begin_libnx_fb();
#endif
}

void deko3d_present_set_status(const char *text) {
#if defined(__SWITCH__)
  std::snprintf(g_status_text, sizeof g_status_text, "%s", text ? text : "");
  g_status_version.fetch_add(1);
#else
  (void)text;
#endif
}

bool deko3d_present_gpu_attached() {
#if defined(__SWITCH__)
  return g_gpu;
#else
  return false;
#endif
}

bool deko3d_present_gpu_image(const void *dk_image, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                              std::uint32_t h) {
#if defined(__SWITCH__)
  if (!g_gpu || !dk_image) return false;
  gt4::perf::Scope present_scope(gt4::perf::kPresent);
  gt4::perf::note_present_size(static_cast<int>(w), static_cast<int>(h));
  return gpu_present_rect(static_cast<const DkImage *>(dk_image), x, y, w, h);
#else
  (void)dk_image;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return false;
#endif
}

} // namespace gt4::platform::nx
