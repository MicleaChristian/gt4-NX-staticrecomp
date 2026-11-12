#include "coregs_nx.hpp"

#include "hw_gs_dispatch.hpp"
#include "hw_gs/backend_dk/writeback_dk.hpp"
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
  auto *backend = g_dispatch->backend();
  backend->write_privileged(0x12000010u, smode1);
  std::uint64_t syncv = 0;
  if (map_crt_syncv(g_crt.mode, syncv)) backend->write_privileged(0x12000060u, syncv);
  backend->write_privileged(0x12000020u, smode2);
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
  return true;
}

void coregs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
  g_crt = {true, interlace, mode, field};
  post_crt();
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
