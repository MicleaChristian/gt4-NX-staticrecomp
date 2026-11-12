// TASK-174: CoreGS writeback drawer on deko3d (1x GPU path for Switch).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dk_gpu.hpp"
#include "hw_batch.hpp"
#include "hw_clut.hpp"
#include "hw_memory.hpp"

namespace hwgs::dk {

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
};

// Fixed GS working surface. Display composes from SLM after a deferred GPU flush.
constexpr int kDkSurface = 1024;

class WritebackDk {
public:
  bool init(const DkHandles &handles, std::string &error);
  void shutdown();
  bool ready() const { return ready_; }

  bool clear_colour(std::uint32_t rgba8, std::string &error);

  // Rasterise one CoreGS batch into the GPU colour target (no SLM round-trip).
  // Returns false → caller should fall back to CPU DrawBatch (and call note_cpu_fallback).
  bool draw_batch(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                  std::string &error);

  // Write dirty GPU pixels back into SLM (call before PCRTC compose / EndFrame).
  bool flush_to_slm(memory::LocalMemory &local, std::string &error);

  // After a CPU DrawBatch fallback, the GPU colour target is no longer authoritative.
  void note_cpu_fallback();

  // EE/GIF wrote into GS memory — drop decoded texture cache entries.
  void invalidate_textures();

  const DkGpuStats &stats() const { return stats_; }
  int surface() const { return kDkSurface; }

private:
  DkHandles handles_{};
  bool ready_ = false;
  DkGpuStats stats_{};
  struct State;
  State *state_ = nullptr;
};

} // namespace hwgs::dk
