#include "coregs_nx.hpp"

#include "hw_gs_dispatch.hpp"
#include "hw_gs/backend_dk/writeback_dk.hpp"
#include "platform/nx/deko3d_present.hpp"

#include "perf_nx.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#if defined(__SWITCH__)
// Runtime (ps2-web-recomp system.cpp), NX-2026-11-27-BF.
namespace ps2 {
void start_vu_thread();
void run_vu_thread();
}
extern "C" {
void gt4_switch_vu_post(std::uint32_t type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
int gt4_switch_on_vu_thread();
int gt4_switch_vu_active();
}
#endif

namespace gt4::switch_app {
namespace {

std::unique_ptr<gt4::hw::Dispatch> g_dispatch;
bool g_active = false;

// NX-2026-11-27-BA: GS thread. Run AZ showed the GPU 9 % busy and every millisecond of a frame
// spent on the one CPU core the emulator used: in the race about 50 ms of EE, IOP and VU1 work
// and about 90 ms of GS work (GIF parsing, batch recording, write-backs) one after the other.
// CoreGS takes input through three calls only (GIF packet, privileged register write, vblank
// present) and returns nothing to the guest, so those calls are copied into a ring and executed
// in order by a second thread on core 1. The main thread waits only when the ring is full or
// when more than one present is still queued (the GS thread is then more than a frame behind).
// Every deko3d call, the present included, happens on the GS thread once it runs.
#if defined(__SWITCH__)
bool g_want_thread = true;
bool g_want_vu_thread = true;
Thread g_vu_thread;
bool g_vu_thread_running = false;
// Command types on the runtime's VU ring that belong to CoreGS (values outside its own enum).
enum : std::uint32_t { kVuGif = 100, kVuPriv = 101, kVuPresent = 102, kVuStop = 103 };

enum : std::uint32_t { kCmdWrap = 0, kCmdGif = 1, kCmdPriv = 2, kCmdPresent = 3, kCmdStop = 4 };
struct CmdHeader {
  std::uint32_t kind;
  std::uint32_t arg;   // GIF path or privileged address
  std::uint64_t value; // privileged value or frame number
  std::uint32_t bytes; // payload bytes after the header
  std::uint32_t pad[3];
};
static_assert(sizeof(CmdHeader) == 32, "ring positions are multiples of 32");

constexpr std::uint64_t kRingBytes = 32ull << 20;
std::unique_ptr<std::uint8_t[]> g_ring;
std::atomic<std::uint64_t> g_head{0}, g_tail{0}; // byte counters, never wrap
std::atomic<std::uint32_t> g_pending_presents{0};
std::atomic<bool> g_consumer_sleeping{false}, g_producer_sleeping{false};
std::mutex g_ring_mutex;
std::condition_variable g_cv_work, g_cv_room;
Thread g_thread;
bool g_thread_running = false;

void gs_execute(const CmdHeader &h, const std::uint8_t *payload) {
  auto *backend = g_dispatch->backend();
  switch (h.kind) {
    case kCmdGif: {
      gt4::perf::Scope scope(gt4::perf::kGif);
      backend->gif_transfer(static_cast<int>(h.arg), payload, h.bytes);
      break;
    }
    case kCmdPriv: backend->write_privileged(h.arg, h.value); break;
    case kCmdPresent: {
      gt4::perf::Scope scope(gt4::perf::kCoreGsFrame);
      backend->set_display_field((h.value & 1u) != 0);
      backend->present(gt4::hw::PresentPhase::progressive);
      backend->next_frame();
      break;
    }
    default: break;
  }
}

void gs_thread_main(void *) {
  gt4::perf::prof_register("gs"); // NX-2026-11-27-BZ
  for (;;) {
    std::uint64_t tail = g_tail.load(std::memory_order_relaxed);
    if (tail == g_head.load(std::memory_order_acquire)) {
      std::unique_lock lock(g_ring_mutex);
      g_consumer_sleeping.store(true);
      // The timeout covers a wake-up sent between the test above and the wait.
      g_cv_work.wait_for(lock, std::chrono::milliseconds(1),
                         [&] { return g_tail.load() != g_head.load(std::memory_order_acquire); });
      g_consumer_sleeping.store(false);
      continue;
    }
    const std::uint64_t pos = tail % kRingBytes;
    CmdHeader h;
    std::memcpy(&h, g_ring.get() + pos, sizeof h);
    if (h.kind == kCmdWrap) {
      g_tail.store(tail + (kRingBytes - pos), std::memory_order_release);
      continue;
    }
    if (h.kind == kCmdStop) {
      g_tail.store(tail + sizeof h, std::memory_order_release);
      return;
    }
    const std::uint64_t t0 = armGetSystemTick();
    gs_execute(h, g_ring.get() + pos + sizeof h);
    gt4::perf::note_gs_thread_busy(armGetSystemTick() - t0);
    g_tail.store(tail + sizeof h + ((h.bytes + 31u) & ~31u), std::memory_order_release);
    if (h.kind == kCmdPresent) g_pending_presents.fetch_sub(1);
    if (g_producer_sleeping.load()) {
      std::lock_guard lock(g_ring_mutex);
      g_cv_room.notify_all();
    }
  }
}

// Main thread: blocks until `ready` holds (the GS thread makes progress in between).
template <typename Ready> void gs_wait(Ready ready) {
  while (!ready()) {
    std::unique_lock lock(g_ring_mutex);
    g_producer_sleeping.store(true);
    g_cv_room.wait_for(lock, std::chrono::milliseconds(1), ready);
    g_producer_sleeping.store(false);
  }
}

void gs_post(std::uint32_t kind, std::uint32_t arg, std::uint64_t value, const std::uint8_t *data, std::uint32_t bytes) {
  const std::uint64_t need = sizeof(CmdHeader) + ((bytes + 31u) & ~31u);
  std::uint64_t head = g_head.load(std::memory_order_relaxed);
  std::uint64_t pos = head % kRingBytes;
  const std::uint64_t pad = pos + need > kRingBytes ? kRingBytes - pos : 0;
  gs_wait([&] { return head + pad + need - g_tail.load(std::memory_order_acquire) <= kRingBytes; });
  if (pad) {
    CmdHeader wrap{};
    wrap.kind = kCmdWrap;
    std::memcpy(g_ring.get() + pos, &wrap, sizeof wrap);
    head += pad;
    pos = 0;
  }
  CmdHeader h{};
  h.kind = kind;
  h.arg = arg;
  h.value = value;
  h.bytes = bytes;
  std::memcpy(g_ring.get() + pos, &h, sizeof h);
  if (bytes) std::memcpy(g_ring.get() + pos + sizeof h, data, bytes);
  g_head.store(head + need, std::memory_order_release);
  if (g_consumer_sleeping.load()) {
    std::lock_guard lock(g_ring_mutex);
    g_cv_work.notify_one();
  }
}

// NX-2026-11-27-BF: VU thread. Run BE at the stock clock: the main thread was 86 to 99 % busy in
// 3D, 45 to 60 ms of each frame in the VU1 programs. The runtime already has a mode that moves
// VIF1 and VU1 to their own thread (its desktop host uses it); it is switched on here. The GS
// queue has one producer: with the VU thread on, that is the VU thread, and what the main thread
// sends to the GS (PATH2/3 packets, privileged writes, the present) goes through the VU ring
// first so it stays in order with the PATH1 packets VU1 emits.
void vu_thread_main(void *) {
  gt4::perf::prof_register("vu"); // NX-2026-11-27-BZ
  ps2::run_vu_thread();
}

void vu_thread_start() {
  if (g_vu_thread_running || !g_want_vu_thread || !g_thread_running) return;
  ps2::start_vu_thread();
  const int main_core = static_cast<int>(svcGetCurrentProcessorNumber());
  const int vu_core = main_core == 2 ? 0 : 2;
  if (R_FAILED(threadCreate(&g_vu_thread, vu_thread_main, nullptr, nullptr, 0x200000, 0x2C, vu_core)) ||
      R_FAILED(threadStart(&g_vu_thread))) {
    // The VU ring is active from here on, so the mode cannot be taken back: stop.
    std::fprintf(stderr, "[gt4-switch] VU thread not started\n");
    std::abort();
  }
  g_vu_thread_running = true;
  std::fprintf(stderr, "[gt4-switch] VU thread running on core %d\n", vu_core);
}

void gs_thread_start() {
  if (g_thread_running || !g_want_thread) return;
  g_ring = std::make_unique<std::uint8_t[]>(kRingBytes);
  g_head = 0;
  g_tail = 0;
  g_pending_presents = 0;
  // Core 1; the main thread stays on its default core. Same priority as the main thread.
  const int main_core = static_cast<int>(svcGetCurrentProcessorNumber());
  const int gs_core = main_core == 1 ? 2 : 1;
  std::fprintf(stderr, "[gt4-switch] main thread on core %d, GS thread on core %d\n", main_core, gs_core);
  if (R_FAILED(threadCreate(&g_thread, gs_thread_main, nullptr, nullptr, 0x400000, 0x2C, gs_core)) ||
      R_FAILED(threadStart(&g_thread))) {
    std::fprintf(stderr, "[gt4-switch] GS thread not started: CoreGS stays on the main thread\n");
    g_ring.reset();
    return;
  }
  g_thread_running = true;
  std::fprintf(stderr, "[gt4-switch] GS thread running on core 1 (queue %llu MiB)\n",
               static_cast<unsigned long long>(kRingBytes >> 20));
}

void gs_thread_stop() {
  if (!g_thread_running) return;
  // With the VU thread on, the stop mark follows everything already queued on the VU ring; the
  // VU thread itself stays parked on its empty ring until the process exits.
  if (g_vu_thread_running) {
    gt4_switch_vu_post(kVuStop, 0, 0, nullptr, 0);
  } else {
    gs_post(kCmdStop, 0, 0, nullptr, 0);
  }
  threadWaitForExit(&g_thread);
  threadClose(&g_thread);
  g_thread_running = false;
  g_ring.reset();
}
// True on the main thread while the VU thread owns the GS queue.
bool via_vu_ring() { return g_vu_thread_running && gt4_switch_on_vu_thread() == 0; }
#else
constexpr bool g_thread_running = false;
void gs_post(std::uint32_t, std::uint32_t, std::uint64_t, const std::uint8_t *, std::uint32_t) {}
void gs_thread_start() {}
void gs_thread_stop() {}
void vu_thread_start() {}
bool via_vu_ring() { return false; }
#endif

// Privileged write from the main thread: queued when the GS thread runs.
void priv_write(std::uint32_t address, std::uint64_t value) {
  if (g_thread_running) {
#if defined(__SWITCH__)
    if (via_vu_ring()) {
      gt4_switch_vu_post(kVuPriv, address, 0, &value, sizeof value);
      return;
    }
#endif
    gs_post(2u, address, value, nullptr, 0);
  } else {
    g_dispatch->backend()->write_privileged(address, value);
  }
}

struct CrtMode {
  bool known = false;
  std::uint32_t interlace = 0, mode = 0, field = 0;
};
CrtMode g_crt;

// Must stay equal to gt4::gs_stream::map_set_gs_crt / map_set_gs_crt_syncv (PC, fixture-tested);
// that translation unit is Win32-only, so the Switch build carries the same table.
// SMODE1: LC bits 3-9, CMOD bits 13-14 (0 progressive, 2 NTSC, 3 PAL). SMODE2: INT bit 0, FFMD bit 1.
bool map_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field, std::uint64_t &smode1,
             std::uint64_t &smode2) {
  constexpr std::uint64_t kCmodProgressive = 0, kCmodNtsc = 2, kCmodPal = 3;
  constexpr std::uint64_t kLcAnalog = 32, kLcHdtv = 22, kLcVga = 15;
  if (field > 1 || interlace > 1) return false;
  std::uint64_t cmod = 0, lc = 0;
  if (mode >= 0x1A && mode <= 0x3B) {
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcVga;
  } else if (mode == 0x02 || mode == 0x03) {
    cmod = mode == 0x02 ? kCmodNtsc : kCmodPal;
    lc = kLcAnalog;
  } else if (mode == 0x50) {
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcAnalog;
  } else if (mode == 0x51) {
    if (!interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcHdtv;
  } else if (mode == 0x52) {
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcHdtv;
  } else {
    return false;
  }
  smode1 = (cmod << 13) | (lc << 3);
  smode2 = interlace ? (1u | (field ? 2u : 0u)) : 0u;
  return true;
}

bool map_crt_syncv(std::uint32_t mode, std::uint64_t &syncv) {
  // SYNCV: VFP bits 0-9, VFPE 10-19, VBP 20-31, VBPE 32-41, VDP 42-52, VS 53-63.
  const auto pack = [](std::uint64_t vs, std::uint64_t vdp, std::uint64_t vbpe, std::uint64_t vbp, std::uint64_t vfpe,
                       std::uint64_t vfp) {
    return (vs << 53) | (vdp << 42) | (vbpe << 32) | (vbp << 20) | (vfpe << 10) | vfp;
  };
  if (mode == 0x02) {
    syncv = pack(6, 480, 6, 26, 6, 1);
    return true;
  }
  if (mode == 0x03) {
    syncv = pack(5, 576, 5, 33, 5, 1);
    return true;
  }
  return false;
}

void post_crt() {
  if (!g_crt.known || !g_active || !g_dispatch || !g_dispatch->backend()) return;
  std::uint64_t smode1 = 0, smode2 = 0;
  if (!map_crt(g_crt.interlace, g_crt.mode, g_crt.field, smode1, smode2)) {
    std::fprintf(stderr, "[gt4-switch] SetGsCrt interlace %u mode 0x%X field %u not mapped; CoreGS display unset\n",
                 g_crt.interlace, g_crt.mode, g_crt.field);
    return;
  }
  priv_write(0x12000010u, smode1);
  std::uint64_t syncv = 0;
  if (map_crt_syncv(g_crt.mode, syncv)) priv_write(0x12000060u, syncv);
  priv_write(0x12000020u, smode2);
  std::fprintf(stderr, "[gt4-switch] CoreGS SMODE1=0x%llX SMODE2=0x%llX\n", static_cast<unsigned long long>(smode1),
               static_cast<unsigned long long>(smode2));
}

// Fixed Switch environment for CoreGS (no getenv on NX for these).
const char *lookup_env(const char *name) {
  static const std::unordered_map<std::string, const char *> kEnv = {
      {"GT4_RENDERER", "hw"},
      {"GT4_HW_CORE", "2"},
      {"GT4_HW_SCALE", "1"},
      {"GT4_HW_DUMP_SOURCE", "scanout"},
  };
  const auto it = kEnv.find(name ? name : "");
  return it == kEnv.end() ? nullptr : it->second;
}

} // namespace

bool coregs_start(bool gpu_draw) {
  coregs_stop();
  hwgs::dk::WritebackDk::set_allowed(gpu_draw);
  if (!gpu_draw) std::fprintf(stderr, "[gt4-switch] gpudraw.off found: CoreGS draws every batch on the CPU\n");
  try {
    g_dispatch = std::make_unique<gt4::hw::Dispatch>(lookup_env);
  } catch (const gt4::hw::OptionError &e) {
    std::fprintf(stderr, "[gt4-switch] CoreGS options: %s\n", e.what());
    g_dispatch.reset();
    return false;
  }
  std::string error;
  if (!g_dispatch->start(error)) {
    std::fprintf(stderr, "[gt4-switch] CoreGS start failed: %s\n", error.c_str());
    g_dispatch.reset();
    return false;
  }
  if (!g_dispatch->hw_active() || !g_dispatch->backend()) {
    std::fprintf(stderr, "[gt4-switch] CoreGS Dispatch not hw-active\n");
    g_dispatch.reset();
    return false;
  }
  std::string attach_err;
  if (!g_dispatch->backend()->attach_window(nullptr, attach_err)) {
    std::fprintf(stderr, "[gt4-switch] CoreGS attach_window: %s\n", attach_err.c_str());
  }
  g_active = true;
  std::fprintf(stderr, "[gt4-switch] CoreGS BackendDk active\n");
  post_crt();
  gs_thread_start();
  vu_thread_start();
  return true;
}

void coregs_set_vu_threaded(bool threaded) {
#if defined(__SWITCH__)
  g_want_vu_thread = threaded;
#else
  (void)threaded;
#endif
}

#if defined(__SWITCH__)
extern "C" void gt4_switch_vu_forward(std::uint32_t type, std::uint32_t arg0, std::uint32_t arg1,
                                      const std::uint8_t *payload, std::uint32_t bytes) {
  (void)arg1;
  switch (type) {
    case kVuGif: gs_post(kCmdGif, arg0, 0, payload, bytes); break;
    case kVuPriv: {
      std::uint64_t value = 0;
      if (bytes >= sizeof value) std::memcpy(&value, payload, sizeof value);
      gs_post(kCmdPriv, arg0, value, nullptr, 0);
      break;
    }
    case kVuPresent: {
      std::uint64_t frame = 0;
      if (bytes >= sizeof frame) std::memcpy(&frame, payload, sizeof frame);
      gs_post(kCmdPresent, 0, frame, nullptr, 0);
      break;
    }
    case kVuStop: gs_post(kCmdStop, 0, 0, nullptr, 0); break;
    default: break;
  }
}
#endif

void coregs_set_threaded(bool threaded) {
#if defined(__SWITCH__)
  g_want_thread = threaded;
#else
  (void)threaded;
#endif
}

void coregs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
  g_crt = {true, interlace, mode, field};
  post_crt();
}

void coregs_stop() {
  gs_thread_stop(); // drains the queue first
  if (g_dispatch) {
    if (g_dispatch->backend()) g_dispatch->backend()->detach_window();
    g_dispatch->stop();
  }
  g_dispatch.reset();
  g_active = false;
}

bool coregs_active() { return g_active && g_dispatch && g_dispatch->hw_active(); }

bool coregs_gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
  if (!coregs_active() || !data || qwc == 0) return false;
  if (g_thread_running) {
#if defined(__SWITCH__)
    if (via_vu_ring()) {
      gt4_switch_vu_post(kVuGif, static_cast<std::uint32_t>(path), 0, data, qwc * 16u);
      return true;
    }
#endif
    gs_post(1u, static_cast<std::uint32_t>(path), 0, data, qwc * 16u);
    return true;
  }
  return g_dispatch->backend()->gif_transfer(path, data, static_cast<std::size_t>(qwc) * 16u);
}

bool coregs_write_privileged(std::uint32_t address, std::uint64_t value) {
  if (!coregs_active()) return false;
  if (g_thread_running) {
#if defined(__SWITCH__)
    if (via_vu_ring()) {
      gt4_switch_vu_post(kVuPriv, address, 0, &value, sizeof value);
      return true;
    }
#endif
    gs_post(2u, address, value, nullptr, 0);
    return true;
  }
  return g_dispatch->backend()->write_privileged(address, value);
}

bool coregs_vblank_present(std::uint64_t frame_osd) {
  if (!coregs_active()) return false;
#if defined(__SWITCH__)
  if (g_thread_running) {
    // At most one present queued behind the one being executed: the guest never runs more than
    // about a frame ahead of the picture.
    g_pending_presents.fetch_add(1);
    if (via_vu_ring()) {
      gt4_switch_vu_post(kVuPresent, 0, 0, &frame_osd, sizeof frame_osd);
    } else {
      gs_post(kCmdPresent, 0, frame_osd, nullptr, 0);
    }
    gs_wait([] { return g_pending_presents.load() <= 1u; });
    return true;
  }
#endif
  auto *backend = g_dispatch->backend();
  // present() -> end_frame() -> scanout() already flush_to_slm once; do not flush twice.
  backend->set_display_field((frame_osd & 1u) != 0);
  backend->present(gt4::hw::PresentPhase::progressive);
  backend->next_frame();
  (void)frame_osd;
  return true;
}

void coregs_fill_perf(std::uint64_t &gif_bytes, std::uint64_t &gpu_draws, std::uint64_t &gpu_batches,
                      std::uint64_t &cpu_fallback, std::uint64_t &tex_cache_hits, std::uint64_t &wb_flushes) {
  gif_bytes = gpu_draws = gpu_batches = cpu_fallback = tex_cache_hits = wb_flushes = 0;
  if (!coregs_active()) return;
  auto *backend = g_dispatch->backend();
  const auto st = backend->stats();
  const auto rc = backend->render_counters();
  gif_bytes = st.gif_bytes;
  gpu_draws = rc.gpu_draws;
  gpu_batches = rc.batches;
  cpu_fallback = rc.gpu_cpu_fallback;
  tex_cache_hits = rc.gpu_tex_cache_hits;
  wb_flushes = rc.gpu_flushes;
}

} // namespace gt4::switch_app
