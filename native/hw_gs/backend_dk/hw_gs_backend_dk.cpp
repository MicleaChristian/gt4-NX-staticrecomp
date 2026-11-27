// TASK-174: CoreGS Backend for Nintendo Switch (deko3d).
// Same gt4::hw::Backend surface as the Vulkan TU; linked only into the Switch NRO.
// GIF / privileged / compose use the CPU reference renderer (correct CoreGS algorithms at 1x).
// Present goes through deko3d staging copyBufferToImage. WritebackDk holds the GPU drawer scaffold.
#include "hw_gs_backend.hpp"

#include "cpuref_draw.hpp"
#include "cpuref_renderer.hpp"
#include "dk_gpu.hpp"
#include "hw_gs_display.hpp"
#include "perf_nx.hpp"
#include "platform/nx/deko3d_present.hpp"
#include "writeback_dk.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <switch.h>

namespace gt4::hw {
namespace {

constexpr std::uint32_t privileged_base = 0x12000000u;
constexpr std::uint32_t privileged_end = 0x12001FFFu;
constexpr std::size_t privileged_slots = (privileged_end - privileged_base + 1) / 8;

std::uint32_t pack_rgba(const hwgs::display::Rgba8 &c) {
  return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
         (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
}

} // namespace

struct Backend::Impl {
  Options options{};
  DeviceReport report{};
  Stats stats{};
  bool initialised = false;
  bool window_attached = false;
  StreamEventHandler stream_handler = nullptr;
  void *stream_context = nullptr;
  std::array<std::uint64_t, privileged_slots> privileged{};
  std::unique_ptr<hwgs::cpuref::CpuRenderer> cpu;
  hwgs::dk::DkHandles dk{};
  hwgs::dk::WritebackDk writeback;
  hwgs::cpuref::FrameImage last_frame{};
  ScanoutRequest last_scanout{}; // packed RGBA8 of last_frame, reused by present_slot
  std::uint64_t present_frames = 0;
  std::uint64_t field_holds = 0; // interlaced FFMD: odd vblanks re-show the previous field
  // Zero-copy present: the deko swapchain shares dk.device; scanout blits on the GPU.
  bool gpu_present = false;
  bool allow_direct = false; // set by end_frame only (other scanout callers want pixels)
  bool last_direct = false;  // last end_frame frame went out through the GPU blit
  // NX-2026-11-27-U: real displayed-frame counters (perf.log fps is guest vblank rate only).
  std::uint32_t disp_fbp = 0xFFFFFFFFu;  // DISPFB fbp of the circuit scanned out this present
  std::uint32_t last_disp_fbp = 0xFFFFFFFFu;
  std::uint64_t disp_flips = 0;          // presents whose DISPFB fbp differs from the previous one
  std::uint64_t draw_presents = 0;       // presents with new GPU draws since the previous present
  std::uint64_t last_draws = 0;
  std::uint64_t win_flips = 0, win_draw_presents = 0, win_presents = 0;
  std::uint64_t win_start_tick = 0;

  void shutdown() {
    if (writeback.ready()) writeback.report(stderr);
    if (gpu_present) {
      gt4::platform::nx::deko3d_present_detach_gpu();
      gpu_present = false;
    }
    writeback.shutdown();
    hwgs::dk::destroy_device(dk);
    cpu.reset();
    initialised = false;
    window_attached = false;
  }
};

Backend::Backend() : impl_(std::make_unique<Impl>()) {}
Backend::~Backend() {
  if (impl_) impl_->shutdown();
}

bool Backend::init(const Options &options, std::string &error) {
  Impl &s = *impl_;
  if (s.initialised) {
    error = "HWR backend already initialised";
    return false;
  }
  s.options = options;
  s.cpu = std::make_unique<hwgs::cpuref::CpuRenderer>();
  if (!hwgs::dk::create_device(s.dk, error)) {
    // Device is optional for headless compose; present still uses platform present.
    std::fprintf(stderr, "[hw-dk] deko3d device: %s (compose still works; present uses soft path)\n",
                 error.c_str());
    error.clear();
  } else {
    std::string wb_err;
    if (!s.writeback.init(s.dk, wb_err)) {
      std::fprintf(stderr, "[hw-dk] WritebackDk: %s\n", wb_err.c_str());
    }
  }
  if (s.writeback.ready()) {
    hwgs::dk::WritebackDk *wb = &s.writeback;
    hwgs::cpuref::CpuRenderer *cpu = s.cpu.get();
    // GPU-only: never call the CPU pixel drawer. A rejected/failed GPU batch is dropped (counted).
    // Soft raster on the Switch ARM cores is what made car select ~0.3 FPS.
    s.cpu->SetBatchOverride(
        [wb](const hwgs::Batch &batch, hwgs::memory::LocalMemory &local, const hwgs::clut::ClutUnit &clut,
             hwgs::cpuref::DrawStats &draw_stats) -> bool {
          std::string err;
          gt4::perf::Scope gpu_draw(gt4::perf::kGsGpuDraw);
          if (wb->draw_batch(batch, local, clut, err)) {
            draw_stats.batches++;
            draw_stats.primitives += batch.prims.size();
          } else {
            wb->note_cpu_fallback(); // name kept: counts batches we refused to soft-draw
          }
          return true;
        });
    s.cpu->SetMemorySyncHook([wb](hwgs::memory::LocalMemory &local, bool display_only) {
      gt4::perf::Scope flush(gt4::perf::kGsFlush);
      std::string err;
      wb->flush_to_slm(local, err, display_only);
    });
    s.cpu->Memory().SetAccessHook([wb, cpu](std::uint32_t block, std::uint32_t blocks, bool write) {
      gt4::perf::Scope flush(gt4::perf::kGsFlush);
      wb->before_memory_access(cpu->Memory().Local(), block, blocks, write);
    });
  }
  s.report.device_name = "deko3d (Switch)";
  s.report.vendor_id = 0x10DE; // Tegra X1
  s.report.max_image_dimension_2d = 4096;
  s.report.max_compute_workgroup_count_x = 65535;
  s.report.optional_found.push_back("cpuref_writeback");
  s.report.optional_found.push_back("deko3d_present");
  if (s.writeback.ready()) s.report.optional_found.push_back("writeback_dk_gpu");
  s.initialised = true;
  std::fprintf(stderr, "[hw-dk] CoreGS BackendDk ready scale=%u (GPU WritebackDk + SLM compose)\n",
               options.scale);
  return true;
}

bool Backend::prepare_shaders(const std::function<void(std::size_t, std::size_t)> &progress,
                              std::string &error) {
  (void)progress;
  (void)error;
  // UAM shader cache for the uber pixel path lands with the full writeback_dk port.
  return true;
}

void Backend::set_stream_event_handler(StreamEventHandler handler, void *context) {
  impl_->stream_handler = handler;
  impl_->stream_context = context;
}

bool Backend::gif_transfer(int path, const std::uint8_t *bytes, std::size_t byte_count) {
  if (path < 1 || path > 3 || !bytes || !impl_->cpu) return false;
  impl_->stats.gif_bytes += byte_count;
  // Transfers inside the packet reach the GPU drawer through the local memory access hook.
  std::vector<std::uint8_t> copy(bytes, bytes + byte_count);
  return impl_->cpu->GifTransfer(static_cast<std::uint32_t>(path), copy);
}

bool Backend::gif_busy(int path) const {
  (void)path;
  return false;
}

void Backend::reset_gif_paths() {
  if (impl_->cpu) impl_->cpu->ResetGif(0x7u);
}

bool Backend::write_privileged(std::uint32_t address, std::uint64_t value) {
  if (address < privileged_base || address > privileged_end || (address & 7u) != 0) {
    impl_->stats.faults++;
    return false;
  }
  impl_->privileged[(address - privileged_base) / 8] = value;
  impl_->stats.privileged_writes++;
  if (impl_->cpu) impl_->cpu->WritePrivileged(address, value);
  return true;
}

std::uint64_t Backend::read_privileged(std::uint32_t address) const {
  if (address < privileged_base || address > privileged_end || (address & 7u) != 0) return 0;
  return impl_->privileged[(address - privileged_base) / 8];
}

void Backend::reset_context() {
  if (impl_->cpu) impl_->cpu->ResetGs(false);
}

void Backend::clear_vram() {
  if (impl_->cpu) impl_->cpu->ResetGs(true);
}

std::size_t Backend::read_transfer_fifo(void *destination, std::size_t qwords) {
  if (!destination || !impl_->cpu) return 0;
  auto data = impl_->cpu->ReadFifo(static_cast<std::uint32_t>(qwords));
  const std::size_t n = std::min(data.size(), qwords * 16);
  std::memcpy(destination, data.data(), n);
  if (n < qwords * 16) std::memset(static_cast<std::uint8_t *>(destination) + n, 0, qwords * 16 - n);
  return qwords;
}

const std::uint8_t *Backend::map_vram(std::uint32_t offset, std::uint32_t size) {
  if (!impl_->cpu || static_cast<std::uint64_t>(offset) + size > 4u * 1024u * 1024u) return nullptr;
  if (impl_->writeback.ready()) {
    std::string err;
    impl_->writeback.flush_to_slm(impl_->cpu->Memory().Local(), err);
  }
  return impl_->cpu->Memory().Local().bytes().data() + offset;
}

void Backend::flush() {
  impl_->stats.flushes++;
  if (impl_->writeback.ready() && impl_->cpu) {
    gt4::perf::Scope flush(gt4::perf::kGsFlush);
    std::string err;
    impl_->writeback.flush_to_slm(impl_->cpu->Memory().Local(), err);
  }
}

void Backend::next_frame() { impl_->stats.frames++; }

void Backend::set_display_field(bool odd) {
  if (impl_->cpu) impl_->cpu->SetDisplayField(odd);
}

void Backend::set_age_targets(bool age) { (void)age; }

bool Backend::has_own_present_queue() const { return false; }

void Backend::wait_idle() {}

bool Backend::scanout(ScanoutRequest &request) {
  request.width = 0;
  request.height = 0;
  request.pixels.clear();
  if (!impl_->cpu) return false;

  // Flush draws first. Prefer copying the GPU colour target straight to the present buffer
  // (skips SLM write-back and the CPU circuit compose). Fall back to colour flush + CPU compose.
  impl_->cpu->FlushBatchesForDisplay();
  hwgs::display::DisplayLayout layout{};
  hwgs::display::PmodeFields pmode{};
  if (!impl_->cpu->PeekDisplay(layout, pmode)) return false;

  // NX-2026-11-13-G: once per 60 peeks — greppable DISPFB vs circuit enable (FMV vs UI hypothesis).
  {
    static std::uint64_t dispfb_diag_n = 0;
    if ((++dispfb_diag_n == 1u) || (dispfb_diag_n % 60u == 0u)) {
      const auto &c0 = layout.circuits[0];
      const auto &c1 = layout.circuits[1];
      std::fprintf(stderr,
                   "[hw-dk] dispfb-diag: c0 en=%d fbp=%u fbw=%u psm=%u rw=%u rh=%u | "
                   "c1 en=%d fbp=%u fbw=%u psm=%u rw=%u rh=%u | pmode en1=%d en2=%d\n",
                   c0.enabled ? 1 : 0, c0.fbp, c0.fbw, c0.psm, c0.read_width, c0.read_height,
                   c1.enabled ? 1 : 0, c1.fbp, c1.fbw, c1.psm, c1.read_width, c1.read_height,
                   pmode.en1 ? 1 : 0, pmode.en2 ? 1 : 0);
      std::fflush(stderr);
    }
  }

  const hwgs::display::CircuitRect &c0 = layout.circuits[0];
  const hwgs::display::CircuitRect &c1 = layout.circuits[1];
  const hwgs::display::MergePlan plan = hwgs::display::plan_merge(pmode, c0, c1, true, true);
  const hwgs::display::CircuitRect *single = nullptr;
  if (plan.use_rc1 && !plan.use_rc2) {
    single = &c0;
  } else if (plan.use_rc2 && !plan.use_rc1) {
    single = &c1;
  }
  impl_->disp_fbp = single != nullptr ? single->fbp : 0xFFFFFFFFu;

  if (impl_->writeback.ready()) {
    // NX-2026-11-13-H: EN2-only (RC2) must GPU-scanout; prior hard-coded circuits[0]/RC1-only.
    if (single != nullptr && single->enabled && single->read_width > 0 && single->read_height > 0) {
      gt4::perf::Scope compose(gt4::perf::kGsCompose);
      std::string err;
      if (impl_->allow_direct && impl_->gpu_present && impl_->window_attached &&
          impl_->writeback.try_present_direct(single->fbp, single->fbw, single->psm, single->dbx, single->dby,
                                              single->read_width, single->read_height, single->display_width,
                                              single->display_height, impl_->cpu->Memory().Local(), err)) {
        request.pixels.clear();
        request.width = single->display_width;
        request.height = single->display_height;
        impl_->last_direct = true;
        impl_->last_frame.valid = true;
        impl_->last_frame.image.width = request.width;
        impl_->last_frame.image.height = request.height;
        return true;
      }
      if (impl_->writeback.try_scanout(single->fbp, single->fbw, single->psm, single->dbx, single->dby,
                                       single->read_width, single->read_height, single->display_width,
                                       single->display_height, request.pixels, impl_->cpu->Memory().Local(),
                                     err)) {
        request.width = single->display_width;
        request.height = single->display_height;
        impl_->last_frame.valid = true;
        impl_->last_frame.image.width = request.width;
        impl_->last_frame.image.height = request.height;
        return true;
      }
    } else {
      // NX-2026-11-13-I: rare — plan is not a single GPU-scanout circuit.
      static std::uint64_t scanout_skip_n = 0;
      if ((++scanout_skip_n % 120u) == 1u) {
        std::fprintf(stderr,
                     "[hw-dk] scanout-skip: plan=outcome=%u use_rc1=%d use_rc2=%d single_read=%d "
                     "en1=%d en2=%d c0_en=%d c1_en=%d\n",
                     static_cast<unsigned>(plan.outcome), plan.use_rc1 ? 1 : 0, plan.use_rc2 ? 1 : 0,
                     plan.single_read ? 1 : 0, pmode.en1 ? 1 : 0, pmode.en2 ? 1 : 0, c0.enabled ? 1 : 0,
                     c1.enabled ? 1 : 0);
        std::fflush(stderr);
      }
    }
  }

  impl_->cpu->SyncForDisplay();

  // NX-2026-11-13-I: ≤64 SLM taps at active DISPFB (prefer c1 when EN2-only) after sync, before compose.
  {
    static std::uint64_t slm_dispfb_n = 0;
    if ((++slm_dispfb_n % 60u) == 1u) {
      const hwgs::display::CircuitRect *sample = nullptr;
      if (pmode.en2 && !pmode.en1 && c1.enabled) {
        sample = &c1;
      } else if (single != nullptr && single->enabled) {
        sample = single;
      } else if (c0.enabled) {
        sample = &c0;
      } else if (c1.enabled) {
        sample = &c1;
      }
      if (sample != nullptr && sample->read_width > 0 && sample->read_height > 0 && sample->dbx >= 0 &&
          sample->dby >= 0) {
        const auto decoded = hwgs::memory::decodePsm(sample->psm);
        if (decoded) {
          const hwgs::memory::BufferView view{hwgs::memory::blockOfPage(sample->fbp), sample->fbw, *decoded};
          hwgs::memory::LocalMemory &local = impl_->cpu->Memory().Local();
          constexpr std::uint32_t kMaxTaps = 64u;
          const std::uint32_t rw = sample->read_width;
          const std::uint32_t rh = sample->read_height;
          const std::uint32_t ox = static_cast<std::uint32_t>(sample->dbx);
          const std::uint32_t oy = static_cast<std::uint32_t>(sample->dby);
          const std::uint32_t step_x = std::max(1u, (rw + 7u) / 8u);
          const std::uint32_t step_y = std::max(1u, (rh + 7u) / 8u);
          std::uint32_t taps = 0;
          std::uint32_t nonzero = 0;
          std::uint32_t max_ch = 0;
          std::int32_t first_x = -1;
          std::int32_t first_y = -1;
          for (std::uint32_t y = 0; y < rh && taps < kMaxTaps; y += step_y) {
            for (std::uint32_t x = 0; x < rw && taps < kMaxTaps; x += step_x) {
              const std::uint32_t word = local.readPixel(view, ox + x, oy + y);
              const std::uint32_t r = word & 0xFFu;
              const std::uint32_t g = (word >> 8) & 0xFFu;
              const std::uint32_t b = (word >> 16) & 0xFFu;
              const std::uint32_t ch = std::max(r, std::max(g, b));
              if (ch > max_ch) max_ch = ch;
              if ((word & 0xFFFFFFu) != 0u) {
                ++nonzero;
                if (first_x < 0) {
                  first_x = static_cast<std::int32_t>(x);
                  first_y = static_cast<std::int32_t>(y);
                }
              }
              ++taps;
            }
          }
          std::fprintf(stderr,
                       "[hw-dk] slm-dispfb: fbp=%u psm=%u nonzero_rgb=%u/%u max=%u first=(%d,%d)\n",
                       sample->fbp, sample->psm, nonzero, taps, max_ch, first_x, first_y);
          std::fflush(stderr);
        }
      }
    }
  }

  gt4::perf::Scope compose(gt4::perf::kGsCompose);
  hwgs::cpuref::FrameImage frame = impl_->cpu->ComposeDisplay(layout, pmode);
  impl_->last_frame = frame;
  if (!frame.valid || frame.image.width == 0 || frame.image.height == 0) return false;
  request.width = frame.image.width;
  request.height = frame.image.height;
  request.pixels.resize(static_cast<std::size_t>(frame.image.width) * frame.image.height);
  for (std::size_t i = 0; i < request.pixels.size(); ++i) {
    request.pixels[i] = pack_rgba(frame.image.pixels[i]);
  }
  return true;
}

bool Backend::attach_window(void *hwnd, std::string &error) {
  (void)hwnd;
  (void)error;
  // Switch has no HWND; present uses the platform swapchain opened by soft_present_begin.
  impl_->window_attached = true;
  // Hand the renderer's device to present: scanout becomes a GPU blit into a deko swapchain.
  // Failure keeps the libnx CPU framebuffer path (attach restores it).
  if (impl_->writeback.ready() && !impl_->gpu_present && impl_->dk.device && impl_->dk.queue) {
    impl_->gpu_present = gt4::platform::nx::deko3d_present_attach_gpu(impl_->dk.device, impl_->dk.queue);
    std::fprintf(stderr, "[hw-dk] gpu present attach: %s\n", impl_->gpu_present ? "ok" : "failed (cpu path)");
    std::fflush(stderr);
  }
  return true;
}

void Backend::detach_window() { impl_->window_attached = false; }

void Backend::resize_window(std::uint32_t width, std::uint32_t height) {
  (void)width;
  (void)height;
}

void Backend::set_display_aspect(bool fill, std::uint32_t ratio_millionths) {
  (void)fill;
  (void)ratio_millionths;
}

void Backend::present(PresentPhase phase) {
  Impl &s = *impl_;
  // Interlaced FFMD (menus): odd guest fields re-show the previous even field. Cuts compose cost
  // in half and stops bob on a progressive panel; race path forces 480p separately.
  const std::uint64_t smode2 = s.cpu ? s.cpu->DisplayRegs().smode2 : 0;
  const bool ffmd = (smode2 & 3u) == 3u;
  if (ffmd && (s.present_frames & 1u) != 0 && s.last_direct && s.last_frame.valid) {
    // Swapchain still shows the even field; no blit, no CPU work.
    ++s.field_holds;
    ++s.present_frames;
    return;
  }
  if (ffmd && (s.present_frames & 1u) != 0 && !s.last_scanout.pixels.empty()) {
    ++s.field_holds;
    present_slot();
    return;
  }
  if (end_frame(phase)) present_slot();
}

bool Backend::end_frame(PresentPhase phase) {
  Impl &s = *impl_;
  s.stats.presents++;
  // Cumulative; repeated so a session ended with Plus still leaves the numbers in stderr.log.
  if (s.writeback.ready() && (s.stats.presents == 1 || s.stats.presents % 60 == 0)) {
    s.writeback.report(stderr);
    std::fprintf(stderr, "[hw-dk] field_holds=%llu\n", static_cast<unsigned long long>(s.field_holds));
  }
  if (s.cpu) s.cpu->SetDisplayField(phase == PresentPhase::field_odd);
  s.last_scanout.phase = phase;
  s.last_direct = false;
  s.allow_direct = true;
  const bool ok = scanout(s.last_scanout);
  s.allow_direct = false;
  // NX-2026-11-27-U: count flips (DISPFB changed) and presents that carry new GPU draws.
  // Draws counted at scanout include the ones flushed for display this present.
  {
    const std::uint64_t draws_now = s.writeback.ready() ? s.writeback.stats().draws : 0;
    const bool new_draws = draws_now != s.last_draws;
    s.last_draws = draws_now;
    const bool flip = s.disp_fbp != s.last_disp_fbp;
    s.last_disp_fbp = s.disp_fbp;
    if (flip) { ++s.disp_flips; ++s.win_flips; }
    if (new_draws) { ++s.draw_presents; ++s.win_draw_presents; }
    ++s.win_presents;
    const std::uint64_t now = armGetSystemTick();
    if (s.win_start_tick == 0) s.win_start_tick = now;
    const double secs = armTicksToNs(now - s.win_start_tick) / 1e9;
    if (secs >= 2.0) {
      std::fprintf(stderr,
                   "[hw-dk] displayed: secs=%.2f presents=%llu (%.1f/s) flips=%llu (%.1f/s) "
                   "presents_with_draws=%llu (%.1f/s) disp_fbp=%u field_holds=%llu\n",
                   secs, static_cast<unsigned long long>(s.win_presents), s.win_presents / secs,
                   static_cast<unsigned long long>(s.win_flips), s.win_flips / secs,
                   static_cast<unsigned long long>(s.win_draw_presents), s.win_draw_presents / secs,
                   s.disp_fbp, static_cast<unsigned long long>(s.field_holds));
      std::fflush(stderr);
      s.win_start_tick = now;
      s.win_presents = s.win_flips = s.win_draw_presents = 0;
    }
  }
  if (!ok || (s.last_scanout.pixels.empty() && !s.last_direct)) {
    s.stats.black_presents++;
    s.last_frame.valid = false;
    return s.window_attached;
  }
  return s.window_attached;
}

void Backend::present_slot() {
  Impl &s = *impl_;
  if (!s.window_attached) return;
  ScanoutRequest &req = s.last_scanout;
  if (s.last_direct && s.last_frame.valid) { // already blitted into the swapchain by scanout()
    ++s.present_frames;
    return;
  }
  if (!s.last_frame.valid || req.width == 0 || req.height == 0 || req.pixels.empty()) {
    gt4::platform::nx::soft_present_heartbeat(s.present_frames++);
    return;
  }
  // NX-2026-11-13-C magenta present probe removed (owner Confirmed magenta on screen → host present OK).
  hwgs::dk::present_rgba(req.pixels.data(), req.width, req.height, s.present_frames++);
}

bool Backend::has_window() const { return impl_->window_attached; }

const Options &Backend::options() const { return impl_->options; }

const DeviceReport &Backend::device_report() const { return impl_->report; }

Stats Backend::stats() const { return impl_->stats; }

RenderCounters Backend::render_counters() const {
  RenderCounters out;
  if (!impl_->cpu) return out;
  const auto &d = impl_->cpu->Stats();
  out.batches = d.batches;
  out.primitives = d.primitives;
  out.primitives_skipped_texture = d.primitivesSkippedTexture;
  out.writeback_pixels = d.framePixelsWritten;
  out.sprites = 0;
  if (impl_->writeback.ready()) {
    const auto &wb = impl_->writeback.stats();
    out.gpu_draws = wb.draws;
    out.gpu_upload_bytes = wb.upload_bytes;
    out.gpu_readback_bytes = wb.readback_bytes;
    out.gpu_submissions = wb.submissions;
    out.batches = wb.batches ? wb.batches : out.batches;
    out.gpu_cpu_fallback = wb.cpu_fallback;
    out.gpu_tex_cache_hits = wb.tex_cache_hits;
    out.gpu_flushes = wb.flushes;
    out.gpu_composed_frames = wb.gpu_scanouts;
  }
  return out;
}

} // namespace gt4::hw
