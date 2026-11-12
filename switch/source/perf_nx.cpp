#include "perf_nx.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::perf {
namespace {

constexpr int kMaxDepth = 8;
constexpr std::uint64_t kLogFrames = 60;
constexpr int kHistBins = 64; // per-window wall ms samples (one per guest frame)

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

std::uint64_t g_slots[kSlotCount];
FILE *g_log = nullptr;
FILE *g_summary = nullptr;
std::string g_app_dir;
std::uint64_t g_window_start = 0;
std::uint64_t g_window_frame = 0;
std::uint64_t g_window_vu1_calls = 0;
std::uint64_t g_prev_gif_bytes = 0;
std::uint64_t g_prev_gpu_draws = 0;
std::uint64_t g_prev_gpu_batches = 0;
std::uint64_t g_prev_cpu_fallback = 0;
std::uint64_t g_prev_tex_hits = 0;
std::uint64_t g_prev_wb_flushes = 0;
std::uint64_t g_prev_aot_compiled = 0;
std::uint64_t g_prev_aot_interpreted = 0;
bool g_window_open = false;

// Per guest frame wall time within the current window (ms).
float g_frame_ms[kHistBins] = {};
int g_frame_ms_count = 0;
std::uint64_t g_last_vblank_ticks = 0;

std::uint32_t g_present_w = 0;
std::uint32_t g_present_h = 0;

// Session totals for session_summary.txt
struct Session {
  std::uint64_t lines = 0;
  double fps_sum = 0;
  double fps_min = 1e9;
  double fps_max = 0;
  double wall_sum = 0;
  double wall_max = 0;
  double vu1_aot_sum = 0;
  double vu1_interp_sum = 0;
  double gif_sum = 0;
  double coregs_sum = 0;
  double soft_sum = 0;
  double present_sum = 0;
  double other_sum = 0;
  std::uint64_t aot_hits = 0;
  std::uint64_t aot_misses = 0;
  std::uint64_t gif_bytes = 0;
  std::uint64_t gpu_draws = 0;
  std::uint64_t cpu_fallback = 0;
  bool saw_coregs = false;
} g_session;

float percentile(float *sorted, int n, float p) {
  if (n <= 0) return 0.f;
  const float idx = p * static_cast<float>(n - 1);
  const int lo = static_cast<int>(idx);
  const int hi = std::min(n - 1, lo + 1);
  const float t = idx - static_cast<float>(lo);
  return sorted[lo] * (1.f - t) + sorted[hi] * t;
}

} // namespace

void scope_begin(int slot) {
  const std::uint64_t now = now_ticks();
  if (t_depth > 0) g_slots[t_stack[t_depth - 1]] += now - t_segment_start;
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

void note_present_size(int width, int height) {
  if (width > 0) g_present_w = static_cast<std::uint32_t>(width);
  if (height > 0) g_present_h = static_cast<std::uint32_t>(height);
}

void open_log(const char *app_dir) {
  close_log();
  if (!app_dir) return;
  g_app_dir = app_dir;
  g_log = std::fopen((g_app_dir + "/perf.log").c_str(), "wb");
  if (!g_log) return;
  std::fprintf(g_log,
               "# GT4Recomp Switch perf.log — one line every %llu guest frames\n"
               "# All *ms columns are milliseconds per guest frame (average over the window).\n"
               "# frame_ms_min/avg/max/p95 = host wall time between vblanks (jitter / latency).\n"
               "# other_ms = EE+IOP+DMA/VIF+IPU and anything not in a named slot.\n"
               "# vu1_aot_hit%% = 100 * aot_calls / (aot_calls + interp_calls) in the window.\n"
               "# Columns:\n"
               "# frame fps wall_ms frame_ms_min frame_ms_avg frame_ms_max frame_ms_p95 "
               "vu1_aot_ms vu1_interp_ms vu1_interp_calls_pf vu1_aot_calls_pf vu1_aot_hit_pct "
               "gif_ms gif_KiB_pf coregs_ms soft_ms present_ms other_ms "
               "present_w present_h gpu_draws_pf gpu_batches_pf cpu_fallback_pf tex_hits_pf wb_flushes_pf "
               "identity_rejects coregs\n",
               static_cast<unsigned long long>(kLogFrames));
  std::fflush(g_log);
  for (auto &s : g_slots) s = 0;
  g_window_open = false;
  g_frame_ms_count = 0;
  g_last_vblank_ticks = 0;
  g_session = {};
  g_prev_gif_bytes = g_prev_gpu_draws = g_prev_gpu_batches = 0;
  g_prev_cpu_fallback = g_prev_tex_hits = g_prev_wb_flushes = 0;
  g_prev_aot_compiled = g_prev_aot_interpreted = 0;
}

void close_log() {
  if (g_log && !g_app_dir.empty()) {
    g_summary = std::fopen((g_app_dir + "/session_summary.txt").c_str(), "wb");
    if (g_summary) {
      const double n = g_session.lines > 0 ? static_cast<double>(g_session.lines) : 1.0;
      std::fprintf(g_summary,
                   "GT4Recomp Switch session summary\n"
                   "windows=%llu  coregs_seen=%d\n"
                   "fps avg=%.2f min=%.2f max=%.2f\n"
                   "wall_ms/frame avg=%.2f max=%.2f\n"
                   "vu1_aot_ms avg=%.2f  vu1_interp_ms avg=%.2f\n"
                   "gif_ms avg=%.2f  coregs_ms avg=%.2f  soft_ms avg=%.2f  present_ms avg=%.2f  other_ms avg=%.2f\n"
                   "vu1_aot_hits=%llu  vu1_aot_misses=%llu  aot_hit_pct=%.1f\n"
                   "gif_bytes=%llu  gpu_draws=%llu  cpu_fallback=%llu\n"
                   "last_present=%ux%u\n"
                   "\nHow to read perf.log: highest *ms column in a scene is the bottleneck.\n"
                   "If vu1_interp_ms is high and vu1_aot_hit_pct is low, AOT is missing entries.\n"
                   "If coregs_ms/soft_ms dominates, GS compose/writeback is the cost.\n"
                   "If present_ms dominates, the host blit/fence path is the cost.\n"
                   "If other_ms dominates on FMV, suspect IPU/EE/CDVD.\n",
                   static_cast<unsigned long long>(g_session.lines), g_session.saw_coregs ? 1 : 0,
                   g_session.fps_sum / n, g_session.fps_min > 1e8 ? 0.0 : g_session.fps_min, g_session.fps_max,
                   g_session.wall_sum / n, g_session.wall_max, g_session.vu1_aot_sum / n, g_session.vu1_interp_sum / n,
                   g_session.gif_sum / n, g_session.coregs_sum / n, g_session.soft_sum / n, g_session.present_sum / n,
                   g_session.other_sum / n, static_cast<unsigned long long>(g_session.aot_hits),
                   static_cast<unsigned long long>(g_session.aot_misses),
                   (g_session.aot_hits + g_session.aot_misses) > 0
                       ? 100.0 * static_cast<double>(g_session.aot_hits) /
                             static_cast<double>(g_session.aot_hits + g_session.aot_misses)
                       : 0.0,
                   static_cast<unsigned long long>(g_session.gif_bytes),
                   static_cast<unsigned long long>(g_session.gpu_draws),
                   static_cast<unsigned long long>(g_session.cpu_fallback), g_present_w, g_present_h);
      std::fclose(g_summary);
      g_summary = nullptr;
    }
  }
  if (g_log) std::fclose(g_log);
  g_log = nullptr;
}

void on_vblank(std::uint64_t guest_frame, std::uint64_t vu1_interpreted_calls, const FrameExtras *extras) {
  if (!g_log) return;
  const std::uint64_t now = now_ticks();

  // Per-frame wall sample (host time since previous vblank).
  if (g_last_vblank_ticks != 0 && g_frame_ms_count < kHistBins) {
    g_frame_ms[g_frame_ms_count++] = static_cast<float>(ticks_to_ms(now - g_last_vblank_ticks));
  }
  g_last_vblank_ticks = now;

  FrameExtras ex{};
  if (extras) ex = *extras;
  if (g_present_w) ex.present_w = g_present_w;
  if (g_present_h) ex.present_h = g_present_h;

  if (!g_window_open) {
    g_window_open = true;
    g_window_frame = guest_frame;
    g_window_vu1_calls = vu1_interpreted_calls;
    g_window_start = now;
    g_prev_gif_bytes = ex.gif_bytes;
    g_prev_gpu_draws = ex.gpu_draws;
    g_prev_gpu_batches = ex.gpu_batches;
    g_prev_cpu_fallback = ex.cpu_fallback;
    g_prev_tex_hits = ex.tex_cache_hits;
    g_prev_wb_flushes = ex.wb_flushes;
    g_prev_aot_compiled = ex.vu1_aot_compiled;
    g_prev_aot_interpreted = ex.vu1_aot_interpreted;
    for (auto &s : g_slots) s = 0;
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
    accounted += slot_ms[i];
  }
  const double fps = wall > 0.0 ? 1000.0 / wall : 0.0;
  const double interp_calls = static_cast<double>(vu1_interpreted_calls - g_window_vu1_calls) / per;

  float sorted[kHistBins];
  const int n_samp = g_frame_ms_count;
  float fmin = 0, favg = 0, fmax = 0, fp95 = 0;
  if (n_samp > 0) {
    std::memcpy(sorted, g_frame_ms, static_cast<std::size_t>(n_samp) * sizeof(float));
    std::sort(sorted, sorted + n_samp);
    fmin = sorted[0];
    fmax = sorted[n_samp - 1];
    float sum = 0;
    for (int i = 0; i < n_samp; ++i) sum += sorted[i];
    favg = sum / static_cast<float>(n_samp);
    fp95 = percentile(sorted, n_samp, 0.95f);
  }

  const double aot_calls_pf =
      static_cast<double>(ex.vu1_aot_compiled - g_prev_aot_compiled) / per;
  const double aot_interp_pf =
      static_cast<double>(ex.vu1_aot_interpreted - g_prev_aot_interpreted) / per;
  const double aot_den = aot_calls_pf + aot_interp_pf;
  const double aot_hit = aot_den > 0.0 ? 100.0 * aot_calls_pf / aot_den : 0.0;

  const double gif_kib =
      static_cast<double>(ex.gif_bytes - g_prev_gif_bytes) / per / 1024.0;
  const double draws_pf = static_cast<double>(ex.gpu_draws - g_prev_gpu_draws) / per;
  const double batches_pf = static_cast<double>(ex.gpu_batches - g_prev_gpu_batches) / per;
  const double fallback_pf = static_cast<double>(ex.cpu_fallback - g_prev_cpu_fallback) / per;
  const double tex_pf = static_cast<double>(ex.tex_cache_hits - g_prev_tex_hits) / per;
  const double flush_pf = static_cast<double>(ex.wb_flushes - g_prev_wb_flushes) / per;
  const double other = wall - accounted;

  std::fprintf(g_log,
               "%llu %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.1f %.1f %.1f %.2f %.1f %.2f %.2f %.2f %.2f "
               "%u %u %.1f %.1f %.1f %.1f %.1f %llu %d\n",
               static_cast<unsigned long long>(guest_frame), fps, wall, fmin, favg, fmax, fp95,
               slot_ms[kVu1Aot], slot_ms[kVu1Interp], interp_calls, aot_calls_pf, aot_hit, slot_ms[kGif], gif_kib,
               slot_ms[kCoreGsFrame], slot_ms[kSoftFrame], slot_ms[kPresent], other, ex.present_w, ex.present_h,
               draws_pf, batches_pf, fallback_pf, tex_pf, flush_pf,
               static_cast<unsigned long long>(ex.vu1_identity_rejects), ex.coregs ? 1 : 0);
  std::fflush(g_log);
  // Short mirror so stderr.log alone is enough when copying only one file off the SD card.
  std::fprintf(stderr,
               "[perf] f=%llu fps=%.1f wall=%.1fms p95=%.1fms aot=%.1fms interp=%.1fms aot_hit=%.0f%% "
               "gif=%.1fms gs=%.1fms present=%.1fms other=%.1fms %ux%u draws=%.0f fb=%.0f\n",
               static_cast<unsigned long long>(guest_frame), fps, wall, fp95, slot_ms[kVu1Aot], slot_ms[kVu1Interp],
               aot_hit, slot_ms[kGif], slot_ms[kCoreGsFrame] + slot_ms[kSoftFrame], slot_ms[kPresent], other,
               ex.present_w, ex.present_h, draws_pf, fallback_pf);
  std::fflush(stderr);

  g_session.lines++;
  g_session.fps_sum += fps;
  g_session.fps_min = std::min(g_session.fps_min, fps);
  g_session.fps_max = std::max(g_session.fps_max, fps);
  g_session.wall_sum += wall;
  g_session.wall_max = std::max(g_session.wall_max, wall);
  g_session.vu1_aot_sum += slot_ms[kVu1Aot];
  g_session.vu1_interp_sum += slot_ms[kVu1Interp];
  g_session.gif_sum += slot_ms[kGif];
  g_session.coregs_sum += slot_ms[kCoreGsFrame];
  g_session.soft_sum += slot_ms[kSoftFrame];
  g_session.present_sum += slot_ms[kPresent];
  g_session.other_sum += other;
  g_session.aot_hits += ex.vu1_aot_compiled - g_prev_aot_compiled;
  g_session.aot_misses += ex.vu1_aot_interpreted - g_prev_aot_interpreted;
  g_session.gif_bytes += ex.gif_bytes - g_prev_gif_bytes;
  g_session.gpu_draws += ex.gpu_draws - g_prev_gpu_draws;
  g_session.cpu_fallback += ex.cpu_fallback - g_prev_cpu_fallback;
  if (ex.coregs) g_session.saw_coregs = true;

  g_window_frame = guest_frame;
  g_window_vu1_calls = vu1_interpreted_calls;
  g_window_start = now;
  g_prev_gif_bytes = ex.gif_bytes;
  g_prev_gpu_draws = ex.gpu_draws;
  g_prev_gpu_batches = ex.gpu_batches;
  g_prev_cpu_fallback = ex.cpu_fallback;
  g_prev_tex_hits = ex.tex_cache_hits;
  g_prev_wb_flushes = ex.wb_flushes;
  g_prev_aot_compiled = ex.vu1_aot_compiled;
  g_prev_aot_interpreted = ex.vu1_aot_interpreted;
  g_frame_ms_count = 0;
}

} // namespace gt4::perf
