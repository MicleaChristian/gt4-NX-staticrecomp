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

std::uint64_t g_slots[kSlotCount];
FILE *g_log = nullptr;
std::string g_app_dir;
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
  FILE *summary = std::fopen((g_app_dir + "/session_summary.txt").c_str(), "wb");
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

void on_vblank(std::uint64_t guest_frame, std::uint64_t vu1_interpreted_calls, const FrameExtras *extras) {
  if (!g_log) return;
  const std::uint64_t now = now_ticks();

  if (g_last_vblank_ticks != 0 && g_frame_ms_count < kHistBins) {
    g_frame_ms[g_frame_ms_count++] = static_cast<float>(ticks_to_ms(now - g_last_vblank_ticks));
  }
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
