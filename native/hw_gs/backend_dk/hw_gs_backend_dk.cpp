// TASK-174: CoreGS Backend for Nintendo Switch (deko3d).
// Same gt4::hw::Backend surface as the Vulkan TU; linked only into the Switch NRO.
// GIF / privileged / compose use the CPU reference renderer (correct CoreGS algorithms at 1x).
// Present goes through deko3d staging copyBufferToImage. WritebackDk holds the GPU drawer scaffold.
#include "hw_gs_backend.hpp"

#include "cpuref_renderer.hpp"
#include "dk_gpu.hpp"
#include "platform/nx/deko3d_present.hpp"
#include "writeback_dk.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

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

  void shutdown() {
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
    s.cpu->SetBatchOverride(
        [wb](const hwgs::Batch &batch, hwgs::memory::LocalMemory &local, const hwgs::clut::ClutUnit &clut,
             hwgs::cpuref::DrawStats &draw_stats) -> bool {
          std::string err;
          if (!wb->draw_batch(batch, local, clut, err)) {
            wb->note_cpu_fallback();
            return false; // CPU DrawBatch
          }
          draw_stats.batches++;
          draw_stats.primitives += batch.prims.size();
          return true;
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
  // Only drop the decoded texture cache when GS local memory actually changed
  // (host IMAGE / BITBLT commit). Register-only GIF packets must not WaitIdle the GPU.
  const std::uint64_t commits_before =
      impl_->cpu->Counters().Get(hwgs::Counter::TransferCommits);
  const std::uint64_t image_before = impl_->cpu->Counters().Get(hwgs::Counter::ImageQwords);
  std::vector<std::uint8_t> copy(bytes, bytes + byte_count);
  const bool ok = impl_->cpu->GifTransfer(static_cast<std::uint32_t>(path), copy);
  if (ok && impl_->writeback.ready()) {
    const std::uint64_t commits_after =
        impl_->cpu->Counters().Get(hwgs::Counter::TransferCommits);
    const std::uint64_t image_after = impl_->cpu->Counters().Get(hwgs::Counter::ImageQwords);
    if (commits_after != commits_before || image_after != image_before) {
      impl_->writeback.invalidate_textures();
    }
  }
  return ok;
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
  return impl_->cpu->Memory().Local().bytes().data() + offset;
}

void Backend::flush() {
  impl_->stats.flushes++;
  if (impl_->writeback.ready() && impl_->cpu) {
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
  if (impl_->writeback.ready()) {
    std::string err;
    impl_->writeback.flush_to_slm(impl_->cpu->Memory().Local(), err);
  }
  hwgs::cpuref::FrameImage frame = impl_->cpu->EndFrame();
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
  if (end_frame(phase)) present_slot();
}

bool Backend::end_frame(PresentPhase phase) {
  Impl &s = *impl_;
  s.stats.presents++;
  if (s.cpu) s.cpu->SetDisplayField(phase == PresentPhase::field_odd);
  s.last_scanout.phase = phase;
  if (!scanout(s.last_scanout) || s.last_scanout.pixels.empty()) {
    s.stats.black_presents++;
    s.last_frame.valid = false;
    return s.window_attached;
  }
  return s.window_attached;
}

void Backend::present_slot() {
  Impl &s = *impl_;
  if (!s.window_attached) return;
  const ScanoutRequest &req = s.last_scanout;
  if (!s.last_frame.valid || req.width == 0 || req.pixels.empty()) {
    gt4::platform::nx::soft_present_heartbeat(s.present_frames++);
    return;
  }
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
  }
  return out;
}

} // namespace gt4::hw
