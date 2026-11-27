#include "perf_nx.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__SWITCH__)
#include <switch.h>
#endif

extern "C" {
// Defined in the runtime's cdvd.cpp (NX-2026-11-27-AZ).
extern unsigned long long gt4_disc_read_ns;
extern unsigned long long gt4_disc_read_bytes;
extern unsigned long long gt4_disc_reads;
// NX-2026-11-27-BH: runtime system.cpp / memory.cpp.
extern unsigned long long gt4_ee_dispatches;
extern unsigned long long gt4_ee_cycles;
extern unsigned long long gt4_ee_slow_calls;
extern unsigned long long gt4_ee_slow_ticks;
extern unsigned long long gt4_ee_io_calls[40];
extern unsigned long long gt4_ee_io_ticks[40];
// NX-2026-11-27-BL: runtime ipu.cpp.
extern unsigned long long gt4_ipu_ticks[4];
extern unsigned long long gt4_ipu_cmds[16];
extern unsigned long long gt4_ipu_stalls;
extern unsigned long long gt4_ipu_kind_ticks[16];
extern unsigned long long gt4_ipu_csc_mb[2];
}

namespace gt4::perf {
namespace {

constexpr int kMaxDepth = 16;
constexpr std::uint64_t kLogFrames = 60;
constexpr int kHistBins = 64; // per-window wall ms samples (one per guest frame)

// CSV column names, indexed by Slot. Keep in the enum's order.
constexpr const char *kSlotNames[kSlotCount] = {
    "ee_ms",       "iop_ms",       "vu0_ms",      "vu1_aot_ms", "vu1_interp_ms", "gif_ms",     "gs_gpu_draw_ms",
    "gs_cpu_draw_ms", "gs_flush_ms", "gs_compose_ms", "coregs_glue_ms", "soft_ms", "present_ms",
};

std::uint64_t now_ticks() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return 0;
#endif
}

double ticks_to_ms(std::uint64_t ticks) {
#if defined(__SWITCH__)
  return static_cast<double>(armTicksToNs(ticks)) / 1.0e6;
#else
  return static_cast<double>(ticks) / 1.0e6;
#endif
}

thread_local int t_stack[kMaxDepth];
thread_local int t_depth = 0;
thread_local std::uint64_t t_segment_start = 0;

// NX-2026-11-27-BA: atomic, the GS thread adds to its slots while the main thread reads them.
std::atomic<std::uint64_t> g_slots[kSlotCount];
std::atomic<std::uint64_t> g_gs_busy{0};
FILE *g_log = nullptr;
std::string g_app_dir;
std::string g_log_tag;
std::uint64_t g_window_start = 0;
std::uint64_t g_window_frame = 0;
std::uint64_t g_window_vu1_calls = 0;
FrameExtras g_prev{};
bool g_window_open = false;

// Per guest frame wall time within the current window (ms).
float g_frame_ms[kHistBins] = {};
int g_frame_ms_count = 0;
std::uint64_t g_last_vblank_ticks = 0;

std::uint32_t g_present_w = 0;
std::uint32_t g_present_h = 0;

// NX-2026-11-27-AZ: hardware use, one "[sys]" line per window in stderr and sys<tag>.log.
//   main_thread   CPU time of the game thread / wall time. The emulator runs on one thread, so
//                 near 100 % means that one core is the limit while the others are unused.
//   core_idle     idle share of the core the game thread runs on.
//   gpu_load      GPU busy share reported by the driver (nvhost-ctrl-gpu PMU), -1 if unavailable.
//   ram           used / total MiB of this process.
//   clocks        CPU / GPU / memory clock in MHz (0 if the service is unavailable).
//   disc          reads, MiB and milliseconds spent reading the disc image in the window.
//   mode          1 docked (boost), 0 handheld.
// Also the slowest guest frame of the window, split by slot.
#if defined(__SWITCH__)
struct Sys {
  bool init = false;
  bool clk_ok = false;
  bool pcv_ok = false;
  ClkrstSession cpu{}, gpu{}, emc{};
  bool nv_ok = false;
  u32 gpu_fd = 0;
  u64 thread_ticks = 0;
  u64 idle_ticks = 0;
  unsigned long long disc_ns = 0, disc_bytes = 0, disc_reads = 0;
  unsigned long long ee_dispatches = 0, ee_cycles = 0, ee_slow_calls = 0, ee_slow_ticks = 0;
  unsigned long long io_calls[40] = {}, io_ticks[40] = {};
  unsigned long long ipu_ticks[4] = {}, ipu_cmds[16] = {}, ipu_stalls = 0;
  unsigned long long ipu_kind[16] = {}, ipu_csc_mb[2] = {};
  FILE *log = nullptr;
} g_sys;

u64 thread_tick_count() {
  u64 v = 0;
  // NX-2026-11-27-BA: the core selector is 64-bit; 0xFFFFFFFF (run AZ) was refused and printed 0 %.
  if (R_FAILED(svcGetInfo(&v, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)))
    svcGetInfo(&v, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX);
  return v;
}

u64 idle_tick_count() {
  u64 v = 0;
  svcGetInfo(&v, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX);
  return v;
}

void sys_init() {
  g_sys.init = true;
  if (hosversionAtLeast(8, 0, 0)) {
    if (R_SUCCEEDED(clkrstInitialize())) {
      g_sys.clk_ok = R_SUCCEEDED(clkrstOpenSession(&g_sys.cpu, PcvModuleId_CpuBus, 3)) &&
                     R_SUCCEEDED(clkrstOpenSession(&g_sys.gpu, PcvModuleId_GPU, 3)) &&
                     R_SUCCEEDED(clkrstOpenSession(&g_sys.emc, PcvModuleId_EMC, 3));
    }
  } else {
    g_sys.pcv_ok = R_SUCCEEDED(pcvInitialize());
  }
  if (R_SUCCEEDED(nvInitialize())) g_sys.nv_ok = R_SUCCEEDED(nvOpen(&g_sys.gpu_fd, "/dev/nvhost-ctrl-gpu"));
  g_sys.thread_ticks = thread_tick_count();
  g_sys.idle_ticks = idle_tick_count();
  g_sys.disc_ns = gt4_disc_read_ns;
  g_sys.disc_bytes = gt4_disc_read_bytes;
  g_sys.disc_reads = gt4_disc_reads;
}

void sys_report(std::uint64_t guest_frame, std::uint64_t window_ticks, float worst_ms, const std::uint64_t *worst_slots,
                int over16, int over33, int over50, int samples) {
  if (!g_sys.init) {
    sys_init();
    return;
  }
  const u64 tt = thread_tick_count(), it = idle_tick_count();
  const double wt = window_ticks > 0 ? static_cast<double>(window_ticks) : 1.0;
  const double main_pct = 100.0 * static_cast<double>(tt - g_sys.thread_ticks) / wt;
  const double idle_pct = 100.0 * static_cast<double>(it - g_sys.idle_ticks) / wt;
  const double gs_pct = 100.0 * static_cast<double>(g_gs_busy.exchange(0)) / wt;
  g_sys.thread_ticks = tt;
  g_sys.idle_ticks = it;

  u64 used = 0, total = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);

  u32 cpu_hz = 0, gpu_hz = 0, emc_hz = 0;
  if (g_sys.clk_ok) {
    clkrstGetClockRate(&g_sys.cpu, &cpu_hz);
    clkrstGetClockRate(&g_sys.gpu, &gpu_hz);
    clkrstGetClockRate(&g_sys.emc, &emc_hz);
  } else if (g_sys.pcv_ok) {
    pcvGetClockRate(PcvModule_CpuBus, &cpu_hz);
    pcvGetClockRate(PcvModule_GPU, &gpu_hz);
    pcvGetClockRate(PcvModule_EMC, &emc_hz);
  }

  double gpu_load = -1.0;
  if (g_sys.nv_ok) {
    u32 load = 0;
    // NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD: GPU busy share in 1/1000.
    if (R_SUCCEEDED(nvIoctl(g_sys.gpu_fd, 0x80044715u, &load))) gpu_load = static_cast<double>(load) / 10.0;
  }

  const unsigned long long dn = gt4_disc_read_ns - g_sys.disc_ns, db = gt4_disc_read_bytes - g_sys.disc_bytes,
                           dr = gt4_disc_reads - g_sys.disc_reads;
  g_sys.disc_ns = gt4_disc_read_ns;
  g_sys.disc_bytes = gt4_disc_read_bytes;
  g_sys.disc_reads = gt4_disc_reads;

  const int docked = appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;

  // NX-2026-11-27-BH: EE line. Per guest frame: guest cycles run in translated code (4.9 M is a
  // whole NTSC frame, less means the EE idled), entries into translated code, memory slow-path
  // calls (hardware registers) and their host time.
  {
    const double f = samples > 0 ? static_cast<double>(samples) : 1.0;
    const unsigned long long dd = gt4_ee_dispatches - g_sys.ee_dispatches, dc = gt4_ee_cycles - g_sys.ee_cycles,
                             sc = gt4_ee_slow_calls - g_sys.ee_slow_calls, st = gt4_ee_slow_ticks - g_sys.ee_slow_ticks;
    g_sys.ee_dispatches = gt4_ee_dispatches;
    g_sys.ee_cycles = gt4_ee_cycles;
    g_sys.ee_slow_calls = gt4_ee_slow_calls;
    g_sys.ee_slow_ticks = gt4_ee_slow_ticks;
    char ee_line[256];
    std::snprintf(ee_line, sizeof ee_line,
                  "[ee-prof] f=%llu cycles=%.2fM/frame dispatches=%.0f/frame slow_io=%.0f/frame slow_io_ms=%.2f/frame",
                  static_cast<unsigned long long>(guest_frame), static_cast<double>(dc) / f / 1.0e6,
                  static_cast<double>(dd) / f, static_cast<double>(sc) / f,
                  static_cast<double>(armTicksToNs(st)) / 1.0e6 / f);
    std::fprintf(stderr, "%s\n", ee_line);
    if (!g_sys.log && !g_app_dir.empty()) g_sys.log = std::fopen((g_app_dir + "/sys" + g_log_tag + ".log").c_str(), "wb");
    if (g_sys.log) std::fprintf(g_sys.log, "%s\n", ee_line);

    // NX-2026-11-27-BI: the six devices with the most slow-path time, per frame:
    // name:calls(ms). ipuR<n> / ipuW<n> = read / write of IPU register 0x10002000 + 4 * n
    // (0 CMD, 4 CTRL, 8 BP, 12 TOP).
    unsigned long long dcalls[40], dticks[40];
    for (int i = 0; i < 40; ++i) {
      dcalls[i] = gt4_ee_io_calls[i] - g_sys.io_calls[i];
      dticks[i] = gt4_ee_io_ticks[i] - g_sys.io_ticks[i];
      g_sys.io_calls[i] = gt4_ee_io_calls[i];
      g_sys.io_ticks[i] = gt4_ee_io_ticks[i];
    }
    static const char *const kNames[8] = {"ipuFifo", "dmac", "gifvif", "gsPriv", "timers", "intcSif", "vuMem", "other"};
    char io_line[320];
    int n = std::snprintf(io_line, sizeof io_line, "[ee-io] f=%llu", static_cast<unsigned long long>(guest_frame));
    for (int shown = 0; shown < 6; ++shown) {
      int best = -1;
      for (int i = 0; i < 40; ++i)
        if (dticks[i] != 0 && (best < 0 || dticks[i] > dticks[best])) best = i;
      if (best < 0 || n <= 0 || n >= static_cast<int>(sizeof io_line) - 48) break;
      char name[16];
      if (best < 16) std::snprintf(name, sizeof name, "ipuR%d", best);
      else if (best < 32) std::snprintf(name, sizeof name, "ipuW%d", best - 16);
      else std::snprintf(name, sizeof name, "%s", kNames[best - 32]);
      n += std::snprintf(io_line + n, sizeof io_line - static_cast<std::size_t>(n), " %s:%.0f(%.2f)", name,
                         static_cast<double>(dcalls[best]) / f, static_cast<double>(armTicksToNs(dticks[best])) / 1.0e6 / f);
      dticks[best] = 0;
    }
    std::fprintf(stderr, "%s\n", io_line);
    if (g_sys.log) std::fprintf(g_sys.log, "%s\n", io_line);

    // NX-2026-11-27-BL: IPU command writes per frame by kind (2 BDEC, 3 VDEC, 4 FDEC, 7 CSC) and
    // the host time of a command write: whole, run_command, drain, top_up (ms per frame).
    unsigned long long it[4], ic[16];
    for (int i = 0; i < 4; ++i) {
      it[i] = gt4_ipu_ticks[i] - g_sys.ipu_ticks[i];
      g_sys.ipu_ticks[i] = gt4_ipu_ticks[i];
    }
    for (int i = 0; i < 16; ++i) {
      ic[i] = gt4_ipu_cmds[i] - g_sys.ipu_cmds[i];
      g_sys.ipu_cmds[i] = gt4_ipu_cmds[i];
    }
    const unsigned long long stalls = gt4_ipu_stalls - g_sys.ipu_stalls;
    g_sys.ipu_stalls = gt4_ipu_stalls;
    if (ic[2] + ic[3] + ic[4] + ic[7] != 0) {
      char ipu_line[320];
      std::snprintf(ipu_line, sizeof ipu_line,
                    "[ipu-prof] f=%llu bdec=%.0f vdec=%.0f fdec=%.0f csc=%.0f /frame | ms/frame: write=%.2f run=%.2f drain=%.2f "
                    "top_up=%.2f | stalls=%.1f/frame",
                    static_cast<unsigned long long>(guest_frame), static_cast<double>(ic[2]) / f, static_cast<double>(ic[3]) / f,
                    static_cast<double>(ic[4]) / f, static_cast<double>(ic[7]) / f,
                    static_cast<double>(armTicksToNs(it[0])) / 1.0e6 / f, static_cast<double>(armTicksToNs(it[1])) / 1.0e6 / f,
                    static_cast<double>(armTicksToNs(it[2])) / 1.0e6 / f, static_cast<double>(armTicksToNs(it[3])) / 1.0e6 / f,
                    static_cast<double>(stalls) / f);
      std::fprintf(stderr, "%s\n", ipu_line);
      if (g_sys.log) std::fprintf(g_sys.log, "%s\n", ipu_line);
      // NX-2026-11-27-BX: decoder time by command (wherever it ran: command write, FIFO write
      // or DMA start), ms per frame, and CSC macroblocks per frame by stream alignment.
      {
        unsigned long long kt[16];
        for (int i = 0; i < 16; ++i) {
          kt[i] = gt4_ipu_kind_ticks[i] - g_sys.ipu_kind[i];
          g_sys.ipu_kind[i] = gt4_ipu_kind_ticks[i];
        }
        const unsigned long long mb0 = gt4_ipu_csc_mb[0] - g_sys.ipu_csc_mb[0];
        const unsigned long long mb1 = gt4_ipu_csc_mb[1] - g_sys.ipu_csc_mb[1];
        g_sys.ipu_csc_mb[0] = gt4_ipu_csc_mb[0];
        g_sys.ipu_csc_mb[1] = gt4_ipu_csc_mb[1];
        char kind_line[256];
        std::snprintf(kind_line, sizeof kind_line,
                      "[ipu-kind] f=%llu ms/frame: bdec=%.2f vdec=%.2f fdec=%.2f csc=%.2f idec=%.2f other=%.2f | csc macroblocks/frame "
                      "aligned=%.0f unaligned=%.0f",
                      static_cast<unsigned long long>(guest_frame), static_cast<double>(armTicksToNs(kt[2])) / 1.0e6 / f,
                      static_cast<double>(armTicksToNs(kt[3])) / 1.0e6 / f, static_cast<double>(armTicksToNs(kt[4])) / 1.0e6 / f,
                      static_cast<double>(armTicksToNs(kt[7])) / 1.0e6 / f, static_cast<double>(armTicksToNs(kt[1])) / 1.0e6 / f,
                      static_cast<double>(armTicksToNs(kt[0] + kt[5] + kt[6] + kt[8] + kt[9])) / 1.0e6 / f,
                      static_cast<double>(mb0) / f, static_cast<double>(mb1) / f);
        std::fprintf(stderr, "%s\n", kind_line);
        if (g_sys.log) std::fprintf(g_sys.log, "%s\n", kind_line);
      }
    }
  }

  char line[640];
  int n = std::snprintf(line, sizeof(line),
                        "[sys] f=%llu main_thread=%.0f%% gs_thread=%.0f%% core_idle=%.0f%% gpu_load=%.0f%% ram=%llu/%lluMiB "
                        "clocks cpu=%u gpu=%u mem=%uMHz disc reads=%llu %.2fMiB %.1fms mode=%d | frames=%d >16.7ms=%d "
                        ">33.3ms=%d >50ms=%d | slowest frame %.1fms:",
                        static_cast<unsigned long long>(guest_frame), main_pct, gs_pct, idle_pct, gpu_load,
                        static_cast<unsigned long long>(used >> 20), static_cast<unsigned long long>(total >> 20),
                        cpu_hz / 1000000u, gpu_hz / 1000000u, emc_hz / 1000000u, dr, static_cast<double>(db) / 1048576.0,
                        static_cast<double>(dn) / 1.0e6, docked, samples, over16, over33, over50, worst_ms);
  int order[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) order[i] = i;
  std::sort(order, order + kSlotCount, [&](int a, int b) { return worst_slots[a] > worst_slots[b]; });
  for (int i = 0; i < 5 && n > 0 && n < static_cast<int>(sizeof(line)) - 40; ++i)
    n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.1f", kSlotNames[order[i]],
                       ticks_to_ms(worst_slots[order[i]]));
  std::fprintf(stderr, "%s\n", line);
  if (!g_sys.log && !g_app_dir.empty()) g_sys.log = std::fopen((g_app_dir + "/sys" + g_log_tag + ".log").c_str(), "wb");
  if (g_sys.log) {
    std::fprintf(g_sys.log, "%s\n", line);
    std::fflush(g_sys.log);
  }
}
#endif

// Slot totals at the last vblank, the slowest frame of the window and its slot split.
std::uint64_t g_slots_at_vblank[kSlotCount] = {};
std::uint64_t g_worst_slots[kSlotCount] = {};
float g_worst_ms = 0;

// Session totals for session_summary.txt
struct Session {
  std::uint64_t lines = 0;
  double fps_sum = 0;
  double fps_min = 1e9;
  double fps_max = 0;
  double wall_sum = 0;
  double wall_max = 0;
  double slot_sum[kSlotCount] = {};
  double other_sum = 0;
  std::uint64_t aot_hits = 0;
  std::uint64_t aot_misses = 0;
  std::uint64_t gif_bytes = 0;
  std::uint64_t gpu_draws = 0;
  std::uint64_t cpu_fallback = 0;
  bool saw_coregs = false;
} g_session;

float percentile(const float *sorted, int n, float p) {
  if (n <= 0) return 0.f;
  const float idx = p * static_cast<float>(n - 1);
  const int lo = static_cast<int>(idx);
  const int hi = std::min(n - 1, lo + 1);
  const float t = idx - static_cast<float>(lo);
  return sorted[lo] * (1.f - t) + sorted[hi] * t;
}

double per_frame(std::uint64_t now, std::uint64_t before, double frames) {
  return static_cast<double>(now - before) / frames;
}

void write_summary() {
  FILE *summary = std::fopen((g_app_dir + "/session_summary" + g_log_tag + ".txt").c_str(), "wb");
  if (!summary) return;
  const double n = g_session.lines > 0 ? static_cast<double>(g_session.lines) : 1.0;
  std::fprintf(summary, "GT4Recomp Switch session summary\n");
  std::fprintf(summary, "windows=%llu  coregs_seen=%d  last_present=%ux%u\n",
               static_cast<unsigned long long>(g_session.lines), g_session.saw_coregs ? 1 : 0, g_present_w,
               g_present_h);
  std::fprintf(summary, "fps avg=%.2f min=%.2f max=%.2f\n", g_session.fps_sum / n,
               g_session.fps_min > 1e8 ? 0.0 : g_session.fps_min, g_session.fps_max);
  std::fprintf(summary, "wall_ms/frame avg=%.2f max=%.2f\n\n", g_session.wall_sum / n, g_session.wall_max);
  std::fprintf(summary, "Average ms per guest frame, largest first:\n");
  int order[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) order[i] = i;
  std::sort(order, order + kSlotCount,
            [](int a, int b) { return g_session.slot_sum[a] > g_session.slot_sum[b]; });
  for (int i = 0; i < kSlotCount; ++i)
    std::fprintf(summary, "  %-16s %8.2f\n", kSlotNames[order[i]], g_session.slot_sum[order[i]] / n);
  std::fprintf(summary, "  %-16s %8.2f\n\n", "other_ms", g_session.other_sum / n);
  const std::uint64_t aot_total = g_session.aot_hits + g_session.aot_misses;
  std::fprintf(summary, "vu1_aot_hits=%llu  vu1_aot_misses=%llu  aot_hit_pct=%.1f\n",
               static_cast<unsigned long long>(g_session.aot_hits),
               static_cast<unsigned long long>(g_session.aot_misses),
               aot_total ? 100.0 * static_cast<double>(g_session.aot_hits) / static_cast<double>(aot_total) : 0.0);
  std::fprintf(summary, "gif_bytes=%llu  gpu_draws=%llu  cpu_fallback_batches=%llu\n",
               static_cast<unsigned long long>(g_session.gif_bytes),
               static_cast<unsigned long long>(g_session.gpu_draws),
               static_cast<unsigned long long>(g_session.cpu_fallback));
  std::fclose(summary);
}

} // namespace

void scope_begin(int slot) {
  const std::uint64_t now = now_ticks();
  if (t_depth > 0 && t_depth <= kMaxDepth) g_slots[t_stack[t_depth - 1]] += now - t_segment_start;
  if (t_depth < kMaxDepth) t_stack[t_depth] = slot;
  ++t_depth;
  t_segment_start = now;
}

void scope_end() {
  const std::uint64_t now = now_ticks();
  if (t_depth <= 0) return;
  --t_depth;
  if (t_depth < kMaxDepth) g_slots[t_stack[t_depth]] += now - t_segment_start;
  t_segment_start = now;
}

void note_gs_thread_busy(std::uint64_t ticks) { g_gs_busy.fetch_add(ticks, std::memory_order_relaxed); }

void note_present_size(int width, int height) {
  if (width > 0) g_present_w = static_cast<std::uint32_t>(width);
  if (height > 0) g_present_h = static_cast<std::uint32_t>(height);
}

void open_log(const char *app_dir, const char *tag) {
  close_log();
  if (!app_dir) return;
  g_app_dir = app_dir;
  g_log_tag = tag ? tag : "";
  g_log = std::fopen((g_app_dir + "/perf" + g_log_tag + ".log").c_str(), "wb");
  if (!g_log) return;
  std::fprintf(g_log,
               "# GT4Recomp Switch perf.log, one line every %llu guest frames.\n"
               "# *_ms columns: exclusive milliseconds per guest frame (a nested slot pauses its parent).\n"
               "# frame_ms_* = host wall time between guest vblanks (jitter). other_ms = time in no slot.\n"
               "# *_pf columns: count per guest frame. aot_hit_pct = VU1 AOT calls / all VU1 calls.\n"
               "frame fps wall_ms frame_ms_min frame_ms_avg frame_ms_max frame_ms_p95",
               static_cast<unsigned long long>(kLogFrames));
  for (const char *name : kSlotNames) std::fprintf(g_log, " %s", name);
  std::fprintf(g_log,
               " other_ms vu1_interp_calls_pf vu1_aot_calls_pf aot_hit_pct gif_KiB_pf gpu_draws_pf gpu_batches_pf"
               " cpu_fallback_pf tex_hits_pf wb_flushes_pf present_w present_h identity_rejects coregs\n");
  std::fflush(g_log);
  for (auto &s : g_slots) s = 0;
  g_window_open = false;
  g_frame_ms_count = 0;
  g_last_vblank_ticks = 0;
  g_session = {};
  g_prev = {};
}

void close_log() {
  if (g_log && !g_app_dir.empty()) write_summary();
  if (g_log) std::fclose(g_log);
  g_log = nullptr;
}

namespace {
std::atomic<unsigned long long> g_prof_frame{0}; // NX-2026-11-27-BZ: guest frame, for the profiler's dump header
}
void on_vblank(std::uint64_t guest_frame, std::uint64_t vu1_interpreted_calls, const FrameExtras *extras) {
  g_prof_frame.store(guest_frame, std::memory_order_relaxed);
  if (!g_log) return;
  const std::uint64_t now = now_ticks();

  if (g_last_vblank_ticks != 0 && g_frame_ms_count < kHistBins) {
    g_frame_ms[g_frame_ms_count++] = static_cast<float>(ticks_to_ms(now - g_last_vblank_ticks));
    // NX-2026-11-27-AZ: remember the slot split of the slowest frame of the window.
    const float ms = g_frame_ms[g_frame_ms_count - 1];
    if (ms > g_worst_ms) {
      g_worst_ms = ms;
      for (int i = 0; i < kSlotCount; ++i) g_worst_slots[i] = g_slots[i].load() - g_slots_at_vblank[i];
    }
  }
  for (int i = 0; i < kSlotCount; ++i) g_slots_at_vblank[i] = g_slots[i];
  g_last_vblank_ticks = now;

  FrameExtras ex{};
  if (extras) ex = *extras;
  ex.present_w = g_present_w;
  ex.present_h = g_present_h;

  if (!g_window_open) {
    g_window_open = true;
    g_window_frame = guest_frame;
    g_window_vu1_calls = vu1_interpreted_calls;
    g_window_start = now;
    g_prev = ex;
    for (auto &s : g_slots) s = 0;
    for (auto &s : g_slots_at_vblank) s = 0;
    g_worst_ms = 0;
    g_frame_ms_count = 0;
    return;
  }

  const std::uint64_t frames = guest_frame - g_window_frame;
  if (frames < kLogFrames) return;

  const double per = static_cast<double>(frames);
  const double wall = ticks_to_ms(now - g_window_start) / per;
  double slot_ms[kSlotCount];
  double accounted = 0.0;
  for (int i = 0; i < kSlotCount; ++i) {
    slot_ms[i] = ticks_to_ms(g_slots[i]) / per;
    g_slots[i] = 0;
    g_slots_at_vblank[i] = 0;
    accounted += slot_ms[i];
  }
#if defined(__SWITCH__)
  {
    int over16 = 0, over33 = 0, over50 = 0;
    for (int i = 0; i < g_frame_ms_count; ++i) {
      if (g_frame_ms[i] > 16.7f) ++over16;
      if (g_frame_ms[i] > 33.3f) ++over33;
      if (g_frame_ms[i] > 50.0f) ++over50;
    }
    sys_report(guest_frame, now - g_window_start, g_worst_ms, g_worst_slots, over16, over33, over50, g_frame_ms_count);
  }
#endif
  g_worst_ms = 0;
  const double fps = wall > 0.0 ? 1000.0 / wall : 0.0;
  const double other = wall - accounted;

  float sorted[kHistBins];
  const int samples = g_frame_ms_count;
  float fmin = 0, favg = 0, fmax = 0, fp95 = 0;
  if (samples > 0) {
    std::memcpy(sorted, g_frame_ms, static_cast<std::size_t>(samples) * sizeof(float));
    std::sort(sorted, sorted + samples);
    fmin = sorted[0];
    fmax = sorted[samples - 1];
    float sum = 0;
    for (int i = 0; i < samples; ++i) sum += sorted[i];
    favg = sum / static_cast<float>(samples);
    fp95 = percentile(sorted, samples, 0.95f);
  }

  const double interp_calls = per_frame(vu1_interpreted_calls, g_window_vu1_calls, per);
  const double aot_calls = per_frame(ex.vu1_aot_compiled, g_prev.vu1_aot_compiled, per);
  const double aot_misses = per_frame(ex.vu1_aot_interpreted, g_prev.vu1_aot_interpreted, per);
  const double aot_hit = (aot_calls + aot_misses) > 0.0 ? 100.0 * aot_calls / (aot_calls + aot_misses) : 0.0;
  const double gif_kib = per_frame(ex.gif_bytes, g_prev.gif_bytes, per) / 1024.0;
  const double draws = per_frame(ex.gpu_draws, g_prev.gpu_draws, per);
  const double batches = per_frame(ex.gpu_batches, g_prev.gpu_batches, per);
  const double fallbacks = per_frame(ex.cpu_fallback, g_prev.cpu_fallback, per);
  const double tex_hits = per_frame(ex.tex_cache_hits, g_prev.tex_cache_hits, per);
  const double flushes = per_frame(ex.wb_flushes, g_prev.wb_flushes, per);

  std::fprintf(g_log, "%llu %.2f %.2f %.2f %.2f %.2f %.2f", static_cast<unsigned long long>(guest_frame), fps, wall,
               fmin, favg, fmax, fp95);
  for (double ms : slot_ms) std::fprintf(g_log, " %.2f", ms);
  std::fprintf(g_log, " %.2f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %u %u %llu %d\n", other, interp_calls,
               aot_calls, aot_hit, gif_kib, draws, batches, fallbacks, tex_hits, flushes, ex.present_w, ex.present_h,
               static_cast<unsigned long long>(ex.vu1_identity_rejects), ex.coregs ? 1 : 0);
  std::fflush(g_log);

  // Short mirror so stderr.log alone is enough: the four largest slots of this window.
  int top[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) top[i] = i;
  std::sort(top, top + kSlotCount, [&](int a, int b) { return slot_ms[a] > slot_ms[b]; });
  std::fprintf(stderr, "[perf] f=%llu fps=%.1f wall=%.1fms p95=%.1fms other=%.1fms top:",
               static_cast<unsigned long long>(guest_frame), fps, wall, fp95, other);
  for (int i = 0; i < 4; ++i) std::fprintf(stderr, " %s=%.1f", kSlotNames[top[i]], slot_ms[top[i]]);
  std::fprintf(stderr, " fallback_pf=%.0f coregs=%d\n", fallbacks, ex.coregs ? 1 : 0);
  std::fflush(stderr);

  g_session.lines++;
  g_session.fps_sum += fps;
  g_session.fps_min = std::min(g_session.fps_min, fps);
  g_session.fps_max = std::max(g_session.fps_max, fps);
  g_session.wall_sum += wall;
  g_session.wall_max = std::max(g_session.wall_max, wall);
  for (int i = 0; i < kSlotCount; ++i) g_session.slot_sum[i] += slot_ms[i];
  g_session.other_sum += other;
  g_session.aot_hits += ex.vu1_aot_compiled - g_prev.vu1_aot_compiled;
  g_session.aot_misses += ex.vu1_aot_interpreted - g_prev.vu1_aot_interpreted;
  g_session.gif_bytes += ex.gif_bytes - g_prev.gif_bytes;
  g_session.gpu_draws += ex.gpu_draws - g_prev.gpu_draws;
  g_session.cpu_fallback += ex.cpu_fallback - g_prev.cpu_fallback;
  if (ex.coregs) g_session.saw_coregs = true;
  if (g_session.lines % 10 == 0) write_summary();

  g_window_frame = guest_frame;
  g_window_vu1_calls = vu1_interpreted_calls;
  g_window_start = now;
  g_prev = ex;
  g_frame_ms_count = 0;
}

} // namespace gt4::perf

// ------------------------------------------------------------------ sampling profiler (BZ)
extern "C" {
extern char __start__[]; // first byte of the program (libnx linker script)
}

namespace gt4::perf {
namespace {

constexpr unsigned kProfThreads = 4;
constexpr std::size_t kProfBuckets = std::size_t{1} << 21; // 64-byte buckets: 128 MiB of code
constexpr std::uint64_t kProfDumpNs = 15'000'000'000ull;

struct ProfThread {
  Handle handle = 0;
  char name[8] = {};
  std::uint32_t *hist = nullptr;
  std::uint64_t total = 0, outside = 0, failed = 0;
};

ProfThread g_prof_threads[kProfThreads];
std::atomic<unsigned> g_prof_count{0};
Thread g_prof_thread;
bool g_prof_started = false;
unsigned g_prof_dumps = 0;
// NX-2026-11-27-CA: start of the program's code in memory. `__start__` read as 0 in this build
// (run BZ: every sample counted as outside), so the base is asked from the kernel: the start of
// the memory region that holds this file's code, which is the first region of the program.
std::uint64_t g_prof_base = 0;

void prof_dump() {
  if (g_app_dir.empty()) return;
  const std::string path = g_app_dir + "/prof" + g_log_tag + ".log";
  std::FILE *f = std::fopen(path.c_str(), g_prof_dumps == 0 ? "wb" : "ab");
  ++g_prof_dumps;
  const unsigned n = std::min(g_prof_count.load(std::memory_order_acquire), kProfThreads);
  if (f) std::fprintf(f, "# dump %u frame=%llu\n", g_prof_dumps, g_prof_frame.load(std::memory_order_relaxed));
  for (unsigned i = 0; i < n; ++i) {
    ProfThread &t = g_prof_threads[i];
    if (!t.hist) continue;
    if (f) {
      std::fprintf(f, "T %s total=%llu outside=%llu failed=%llu\n", t.name, static_cast<unsigned long long>(t.total),
                   static_cast<unsigned long long>(t.outside), static_cast<unsigned long long>(t.failed));
      for (std::size_t b = 0; b < kProfBuckets; ++b) {
        if (t.hist[b] >= 2u) std::fprintf(f, "%s %zx %u\n", t.name, b * 64u, t.hist[b]);
      }
    }
    std::memset(t.hist, 0, kProfBuckets * sizeof(std::uint32_t));
    t.total = t.outside = t.failed = 0;
  }
  if (f) std::fclose(f);
}

void prof_thread_main(void *) {
  const std::uint64_t base = g_prof_base;
  std::uint64_t last_dump = armGetSystemTick();
  for (;;) {
    svcSleepThread(1'000'000); // 1 ms
    const unsigned n = std::min(g_prof_count.load(std::memory_order_acquire), kProfThreads);
    for (unsigned i = 0; i < n; ++i) {
      ProfThread &t = g_prof_threads[i];
      if (!t.hist || !t.handle) continue;
      // Nothing between the pause and the resume may take a lock: the stopped thread can hold any.
      if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) {
        ++t.failed;
        continue;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, t.handle);
      svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
      if (R_FAILED(rc)) {
        ++t.failed;
        continue;
      }
      ++t.total;
      const std::uint64_t off = ctx.pc.x - base;
      if (off < kProfBuckets * 64u) ++t.hist[off >> 6];
      else ++t.outside;
    }
    const std::uint64_t now = armGetSystemTick();
    if (armTicksToNs(now - last_dump) >= kProfDumpNs) {
      prof_dump();
      last_dump = armGetSystemTick();
    }
  }
}

} // namespace

void prof_register(const char *name) {
  const unsigned i = g_prof_count.load(std::memory_order_relaxed);
  if (i >= kProfThreads) return;
  ProfThread &t = g_prof_threads[i];
  t.hist = static_cast<std::uint32_t *>(std::calloc(kProfBuckets, sizeof(std::uint32_t)));
  if (!t.hist) return;
  std::snprintf(t.name, sizeof t.name, "%s", name);
  t.handle = threadGetCurHandle();
  g_prof_count.store(i + 1, std::memory_order_release);
}

void prof_start() {
  if (g_prof_started || g_app_dir.empty()) return;
  if (std::FILE *off = std::fopen((g_app_dir + "/prof.off").c_str(), "rb")) {
    std::fclose(off);
    std::fprintf(stderr, "[prof] prof.off found: sampling profiler off\n");
    return;
  }
  {
    MemoryInfo info{};
    u32 page = 0;
    const std::uint64_t here = reinterpret_cast<std::uint64_t>(&prof_start);
    if (R_FAILED(svcQueryMemory(&info, &page, here)) || info.addr == 0 || info.addr > here) {
      std::fprintf(stderr, "[prof] code region not found: sampling profiler off\n");
      return;
    }
    g_prof_base = info.addr;
    std::fprintf(stderr, "[prof] code region %p size %llx; prof_start at %p (offset %llx)\n", reinterpret_cast<void *>(info.addr),
                 static_cast<unsigned long long>(info.size), reinterpret_cast<void *>(here),
                 static_cast<unsigned long long>(here - info.addr));
  }
  // Priority above the sampled threads (0x2C) so the 1 ms sleep is kept; default core.
  if (R_FAILED(threadCreate(&g_prof_thread, prof_thread_main, nullptr, nullptr, 0x10000, 0x2B, -2)) ||
      R_FAILED(threadStart(&g_prof_thread))) {
    std::fprintf(stderr, "[prof] sampler thread not started\n");
    return;
  }
  g_prof_started = true;
  std::fprintf(stderr, "[prof] sampling profiler on: prof%s.log every 15 s, offsets relative to %p\n", g_log_tag.c_str(),
               static_cast<void *>(&__start__[0]));
}

} // namespace gt4::perf
