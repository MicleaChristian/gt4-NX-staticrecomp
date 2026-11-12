#include "coregs_nx.hpp"

#include "hw_gs_dispatch.hpp"
#include "platform/nx/deko3d_present.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace gt4::switch_app {
namespace {

std::unique_ptr<gt4::hw::Dispatch> g_dispatch;
bool g_active = false;

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

bool coregs_start() {
  coregs_stop();
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
  return true;
}

void coregs_stop() {
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
  return g_dispatch->backend()->gif_transfer(path, data, static_cast<std::size_t>(qwc) * 16u);
}

bool coregs_write_privileged(std::uint32_t address, std::uint64_t value) {
  if (!coregs_active()) return false;
  return g_dispatch->backend()->write_privileged(address, value);
}

bool coregs_vblank_present(std::uint64_t frame_osd) {
  if (!coregs_active()) return false;
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
