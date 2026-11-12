// TASK-174: device perf meter for the Switch NRO.
// Writes perf.log (CSV, one line / ~60 guest frames) and session_summary.txt on exit.
#pragma once

#include <cstdint>

namespace gt4::perf {

enum Slot : int {
  kVu1Aot = 0,      // VU1 AOT compiled path
  kVu1Interp = 1,   // VU1 interpreter fallback
  kGif = 2,         // gif::transfer (CoreGS or donor)
  kCoreGsFrame = 3, // CoreGS flush + compose + present at vblank
  kSoftFrame = 4,   // donor display_frame readback
  kPresent = 5,     // host present (GPU blit / libnx)
  kSlotCount = 6,
};

// PC overlay vocabulary (scripts/vu1_aot_overlay.inc).
enum class Category : int {
  vu0_interpreter = 0,
  vu1_interpreter = 1,
  vu1_aot = 2,
};

inline Category vu_interpreter_category(int unit) {
  return unit == 1 ? Category::vu1_interpreter : Category::vu0_interpreter;
}

void scope_begin(int slot);
void scope_end();

struct Scope {
  explicit Scope(int slot) { scope_begin(slot); }
  explicit Scope(Category category) {
    if (category == Category::vu1_aot) scope_begin(kVu1Aot);
    else scope_begin(kVu1Interp); // VU0 rare; fold into interpret bucket
  }
  ~Scope() { scope_end(); }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};

// Optional snapshot filled by the vblank hook (CoreGS / present / AOT counters).
struct FrameExtras {
  std::uint32_t present_w = 0;
  std::uint32_t present_h = 0;
  std::uint64_t gif_bytes = 0;          // cumulative; delta logged per window
  std::uint64_t gpu_draws = 0;
  std::uint64_t gpu_batches = 0;
  std::uint64_t cpu_fallback = 0;
  std::uint64_t tex_cache_hits = 0;
  std::uint64_t wb_flushes = 0;
  std::uint64_t vu1_aot_compiled = 0;   // cumulative AOT hits
  std::uint64_t vu1_aot_interpreted = 0;
  std::uint64_t vu1_identity_rejects = 0;
  bool coregs = false;
};

void open_log(const char *app_dir);
void close_log();
// Called once per guest vblank. `extras` may be null (zeros).
void on_vblank(std::uint64_t guest_frame, std::uint64_t vu1_interpreted_calls, const FrameExtras *extras);

// Present path reports the last guest frame size (for the CSV).
void note_present_size(int width, int height);

} // namespace gt4::perf
