// TASK-174: device perf meter for the Switch NRO.
// Writes perf.log (one line / 60 guest frames) and session_summary.txt on exit.
// Every object that includes this header must list it as a make prerequisite (runtime.mk):
// a stale object keeps the old slot numbers and books its time into the wrong column.
#pragma once

#include <cstdint>

namespace gt4::perf {

// Exclusive time slots: a nested scope pauses its parent, so the columns add up to the frame.
enum Slot : int {
  kEe = 0,          // translated EE code (dispatch), including IPU/DMA work it triggers directly
  kIop = 1,         // IOP interpreter (sync_iop)
  kVu0 = 2,         // VU0 micro programs (VCALLMS, interpreted)
  kVu1Aot = 3,      // VU1 AOT compiled programs
  kVu1Interp = 4,   // VU1 interpreter fallback
  kGif = 5,         // GIF packets: CoreGS front end (parse, batching) or donor CPU GS raster
  kGsGpuDraw = 6,   // CoreGS: WritebackDk batch recording on deko3d
  kGsCpuDraw = 7,   // CoreGS: CPU DrawBatch fallback (batch the GPU path refused)
  kGsFlush = 8,     // CoreGS: GPU colour target read back into GS memory (includes GPU wait)
  kGsCompose = 9,   // CoreGS: CPU display compose (EndFrame) + RGBA pack
  kCoreGsFrame = 10,// CoreGS vblank glue not covered by the slots above
  kSoftFrame = 11,  // donor display_frame readback (coregs.off only)
  kPresent = 12,    // host present (GPU blit / libnx)
  kSlotCount = 13,
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
    else if (category == Category::vu1_interpreter) scope_begin(kVu1Interp);
    else scope_begin(kVu0);
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

void open_log(const char *app_dir, const char *tag = "");
void close_log();
// Called once per guest vblank. `extras` may be null (zeros).
void on_vblank(std::uint64_t guest_frame, std::uint64_t vu1_interpreted_calls, const FrameExtras *extras);

// Present path reports the last guest frame size (for the CSV).
void note_present_size(int width, int height);

// NX-2026-11-27-BA: ticks the GS thread spent executing commands (see coregs_nx.cpp). Reported as
// gs_thread in the [sys] line. With the GS thread on, the GIF / GS slots are measured on that
// thread and run at the same time as the EE slots, so the slot columns can add up to more than
// the frame; coregs_glue_ms on the main thread is then the time it waited for the GS thread.
void note_gs_thread_busy(std::uint64_t ticks);

// NX-2026-11-27-BZ: sampling profiler. A helper thread stops each registered thread for an
// instant about 1000 times a second, reads where it was executing, and counts it in 64-byte
// buckets of the program's code. Every 15 seconds the counts go to prof<tag>.log (one line per
// bucket: thread name, offset in the program, samples) and start again from zero. The offsets
// are those of gt4recomp.elf, so addr2line names the function. An empty file `prof.off` next to
// the program turns it off. A thread calls prof_register itself, once.
void prof_register(const char *name);
void prof_start();

} // namespace gt4::perf
