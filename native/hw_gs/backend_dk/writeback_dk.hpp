// TASK-174: CoreGS writeback drawer on deko3d (1x GPU path for Switch).
//
// The GPU holds a copy of one frame buffer (the "surface") seeded from GS local memory (SLM).
// Batches it can draw exactly enough are rasterised there; everything else falls back to the
// CPU reference drawer. Ordering contract with the CPU side:
// - flush_to_slm writes GPU-drawn pixels back before anything reads SLM (CPU draw, display
//   compose, FIFO read, transfer that touches the surface);
// - memory_written tells the drawer that SLM changed behind it (CPU draw, transfer), so the
//   GPU copy is re-seeded and decoded textures are dropped.
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
  ZWrite,       // depth writes enabled (no GPU depth buffer yet)
  ZTest,        // depth test other than ALWAYS
  AlphaTest,    // alpha test with a fail action that still writes
  DestAlpha,    // DATE or FBA
  Blend,        // blend equation not in the supported set, or PABE
  Fog,          // FGE
  TexFunction,  // TFX highlight modes
  FramePsm,     // frame format other than CT32/CT24
  FrameMask,    // FBMSK != 0
  PrimType,     // points or lines
  Wrap,         // region clamp / region repeat
  Mip,          // mipmapped sampling
  Texture,      // texture format or size the decoder does not handle
  Surface,      // draw area outside the 1024x1024 GPU surface
  Other,        // AA1, COLCLAMP=0, unsafe Q
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
  std::uint64_t rejects[static_cast<int>(Reject::Count)] = {};
};

// Fixed GS working surface. Display composes from SLM after a deferred GPU flush.
constexpr int kDkSurface = 1024;

class WritebackDk {
public:
  bool init(const DkHandles &handles, std::string &error);
  void shutdown();
  bool ready() const { return ready_; }

  bool clear_colour(std::uint32_t rgba8, std::string &error);

  // Rasterise one CoreGS batch on the GPU. Returns false (with no state change visible to SLM)
  // when the batch must be drawn by the CPU; the caller then flushes, draws and calls
  // note_cpu_fallback.
  bool draw_batch(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                  std::string &error);

  // Write dirty GPU pixels back into SLM.
  bool flush_to_slm(memory::LocalMemory &local, std::string &error);

  // A transfer is about to access [block, block + blocks). Writes GPU pixels back when the range
  // overlaps them, and forgets the GPU copy when the transfer writes over it.
  void before_memory_access(memory::LocalMemory &local, std::uint32_t block, std::uint32_t blocks, bool write);

  // After a CPU DrawBatch fallback: SLM changed, the GPU copy and decoded textures are stale.
  void note_cpu_fallback();

  void invalidate_textures();

  void report(std::FILE *out) const;

  const DkGpuStats &stats() const { return stats_; }
  int surface() const { return kDkSurface; }

private:
  void memory_written();

  DkHandles handles_{};
  bool ready_ = false;
  DkGpuStats stats_{};
  struct State;
  State *state_ = nullptr;
};

} // namespace hwgs::dk
