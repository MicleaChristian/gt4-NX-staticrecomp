// TASK-174: CoreGS writeback drawer on deko3d (1x GPU path for Switch).
//
// The GPU holds copies of the current frame buffer and Z buffer ("surfaces"), seeded from GS
// local memory (SLM). Batches the GPU path reproduces are rasterised there; the rest fall back to
// the CPU reference drawer. Ordering contract with the CPU side:
// - GPU-drawn pixels are written back before anything reads SLM: a CPU draw, the display compose
//   (colour only), a FIFO read, a texture or transfer that overlaps a surface;
// - when SLM changes behind the drawer (CPU draw, transfer), the GPU copies are re-seeded and
//   decoded textures dropped.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dk_gpu.hpp"
#include "hw_batch.hpp"
#include "hw_clut.hpp"
#include "hw_memory.hpp"

namespace hwgs::dk {

// Why a batch went to the CPU drawer. Counted per reason for stderr.log.
enum class Reject : int {
  DepthUnavailable, // depth test or write while the start-up depth self-test failed
  ZFormat,          // Z buffer format the depth path does not handle
  AlphaFailBlend,   // alpha test fail action that needs a second pass, combined with blending
  DestAlphaTest,    // DATE
  Blend,            // blend equation not expressible with GPU blend factors, Ad coefficient, or PABE
  ColClamp,         // COLCLAMP = 0 (wrap instead of clamp)
  FramePsm,         // frame format other than CT32/CT24
  FrameMask,        // FBMSK != 0
  PrimMix,          // lines mixed with other primitives
  Texture,          // texture format or size the decoder does not handle
  Surface,          // draw area outside the 1024x1024 GPU surface
  Other,            // AA1, unsafe Q, vertex overflow
  Count
};

struct DkGpuStats {
  std::uint64_t submissions = 0;
  std::uint64_t draws = 0;
  std::uint64_t vertices = 0;
  std::uint64_t batches = 0;
  std::uint64_t clears = 0;
  std::uint64_t present_frames = 0;
  std::uint64_t upload_bytes = 0;
  std::uint64_t readback_bytes = 0;
  std::uint64_t cpu_fallback = 0;
  std::uint64_t tex_cache_hits = 0;
  std::uint64_t flushes = 0;
  std::uint64_t seeds = 0;
  std::uint64_t depth_batches = 0;
  std::uint64_t mip_base_level = 0; // mipmapped batches drawn from the base level (approximation)
  std::uint64_t rejects[static_cast<int>(Reject::Count)] = {};
};

// Fixed GS working surface. Display composes from SLM after a deferred GPU flush.
constexpr int kDkSurface = 1024;

class WritebackDk {
public:
  bool init(const DkHandles &handles, std::string &error);
  void shutdown();
  bool ready() const { return ready_; }

  // Rasterise one CoreGS batch on the GPU. Returns false when the batch must be drawn by the CPU;
  // the caller then calls flush_to_slm, draws, and calls note_cpu_fallback.
  bool draw_batch(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                  std::string &error);

  // Write dirty GPU pixels back into SLM: colour and depth, or colour only (display compose).
  bool flush_to_slm(memory::LocalMemory &local, std::string &error, bool colour_only = false);

  // A transfer is about to access [block, block + blocks). Writes GPU pixels back when the range
  // overlaps them, and forgets the GPU copies when the transfer writes over them.
  void before_memory_access(memory::LocalMemory &local, std::uint32_t block, std::uint32_t blocks, bool write);

  // After a CPU DrawBatch fallback: SLM changed, the GPU copies and decoded textures are stale.
  void note_cpu_fallback();

  void invalidate_textures();

  void report(std::FILE *out) const;

  const DkGpuStats &stats() const { return stats_; }
  int surface() const { return kDkSurface; }

  struct State; // deko3d objects; defined in writeback_dk.cpp only

private:
  void memory_written();
  bool self_test(std::string &error);

  DkHandles handles_{};
  bool ready_ = false;
  DkGpuStats stats_{};
  State *state_ = nullptr;
};

} // namespace hwgs::dk
