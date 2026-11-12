#include "writeback_dk.hpp"

#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(__SWITCH__)
#include <deko3d.h>

#include "gs_draw_fsh_dksh.hpp"
#include "gs_draw_vsh_dksh.hpp"
#endif

namespace hwgs::dk {
namespace {

struct GpuVert {
  float x, y;
  float r, g, b, a;
  float u, v;
};

struct TexKey {
  std::uint64_t tex0 = 0;
  std::uint64_t texa = 0;
  std::uint32_t clut_gen = 0;
  bool operator==(const TexKey &o) const {
    return tex0 == o.tex0 && texa == o.texa && clut_gen == o.clut_gen;
  }
};

struct TexKeyHash {
  std::size_t operator()(const TexKey &k) const {
    return static_cast<std::size_t>(k.tex0 ^ (k.texa * 0x9E3779B97F4A7C15ull) ^
                                    (static_cast<std::uint64_t>(k.clut_gen) << 17));
  }
};

#if defined(__SWITCH__)
struct DkshHeader {
  uint32_t magic;
  uint32_t header_sz;
  uint32_t control_sz;
  uint32_t code_sz;
  uint32_t programs_off;
  uint32_t num_programs;
};

bool load_shader(DkMemBlock code_mem, uint32_t &code_off, const std::uint8_t *bytes, std::size_t size,
                 DkShader &out, std::string &error) {
  if (size < sizeof(DkshHeader)) {
    error = "dksh too small";
    return false;
  }
  DkshHeader hdr{};
  std::memcpy(&hdr, bytes, sizeof(hdr));
  if (hdr.control_sz > size || hdr.code_sz + hdr.control_sz > size) {
    error = "dksh truncated";
    return false;
  }
  std::vector<std::uint8_t> control(hdr.control_sz);
  std::memcpy(control.data(), bytes, hdr.control_sz);
  code_off = (code_off + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
  std::memcpy(static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(code_mem)) + code_off, bytes + hdr.control_sz,
              hdr.code_sz);
  DkShaderMaker maker;
  dkShaderMakerDefaults(&maker, code_mem, code_off);
  maker.control = control.data();
  maker.programId = 0;
  dkShaderInitialize(&out, &maker);
  if (!dkShaderIsValid(&out)) {
    error = "dkShaderInitialize failed";
    return false;
  }
  code_off += hdr.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE;
  return true;
}
#endif

float ndc_x(float px) { return px / static_cast<float>(kDkSurface) * 2.f - 1.f; }
float ndc_y(float py) { return -(py / static_cast<float>(kDkSurface) * 2.f - 1.f); }

GpuVert make_vert(const hwgs::Vertex &v, bool fst, float tex_w, float tex_h) {
  GpuVert o{};
  o.x = ndc_x(static_cast<float>(v.x4) / 16.f);
  o.y = ndc_y(static_cast<float>(v.y4) / 16.f);
  o.r = v.r / 255.f;
  o.g = v.g / 255.f;
  o.b = v.b / 255.f;
  o.a = v.a / 255.f;
  if (fst && tex_w > 0.f && tex_h > 0.f) {
    o.u = (static_cast<float>(v.u) / 16.f) / tex_w;
    o.v = (static_cast<float>(v.v) / 16.f) / tex_h;
  } else {
    o.u = v.s;
    o.v = v.t;
  }
  return o;
}

bool bit(uint64_t prim, unsigned pos) { return ((prim >> pos) & 1u) != 0; }

void expand_prims(const hwgs::Batch &batch, bool fst, float tex_w, float tex_h, std::vector<GpuVert> &verts) {
  for (const auto &prim : batch.prims) {
    if (prim.first + prim.count > batch.vertices.size()) continue;
    const auto *pv = batch.vertices.data() + prim.first;
    switch (prim.type) {
    case hwgs::PrimType::Point:
      if (prim.count >= 1) {
        auto v = make_vert(pv[0], fst, tex_w, tex_h);
        verts.push_back(v);
        verts.push_back(v);
        verts.push_back(v);
      }
      break;
    case hwgs::PrimType::Triangle:
      if (prim.count >= 3) {
        verts.push_back(make_vert(pv[0], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[1], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[2], fst, tex_w, tex_h));
      }
      break;
    case hwgs::PrimType::TriangleStrip:
      for (uint32_t i = 2; i < prim.count; ++i) {
        verts.push_back(make_vert(pv[i - 2], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[i - 1], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[i], fst, tex_w, tex_h));
      }
      break;
    case hwgs::PrimType::TriangleFan:
      for (uint32_t i = 2; i < prim.count; ++i) {
        verts.push_back(make_vert(pv[0], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[i - 1], fst, tex_w, tex_h));
        verts.push_back(make_vert(pv[i], fst, tex_w, tex_h));
      }
      break;
    case hwgs::PrimType::Sprite:
      if (prim.count >= 2) {
        auto a = make_vert(pv[0], fst, tex_w, tex_h);
        auto b = make_vert(pv[1], fst, tex_w, tex_h);
        GpuVert v00 = a, v11 = b, v01 = a, v10 = a;
        v01.y = b.y;
        v01.v = b.v;
        v10.x = b.x;
        v10.u = b.u;
        verts.push_back(v00);
        verts.push_back(v10);
        verts.push_back(v11);
        verts.push_back(v00);
        verts.push_back(v11);
        verts.push_back(v01);
      }
      break;
    default:
      break;
    }
  }
}

bool decode_texture(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                    std::vector<std::uint32_t> &out, uint32_t &tw, uint32_t &th, std::string &error) {
  const uint64_t tex0 = batch.state.ctx.tex0;
  const uint32_t psm_code = hwgs::tex0::Psm(tex0);
  const auto psm = memory::decodePsm(psm_code);
  if (!psm) {
    error = "unsupported texture PSM";
    return false;
  }
  tw = 1u << hwgs::tex0::Tw(tex0);
  th = 1u << hwgs::tex0::Th(tex0);
  if (tw == 0 || th == 0 || tw > 1024 || th > 1024) {
    error = "texture axis unusable";
    return false;
  }
  memory::BufferView view{hwgs::tex0::Tbp0(tex0), hwgs::tex0::Tbw(tex0), *psm};
  out.resize(static_cast<std::size_t>(tw) * th);
  if (hwgs::IsIndexedPsm(psm_code)) {
    const auto pal =
        clut.Decode(psm_code, hwgs::tex0::Cpsm(tex0), hwgs::tex0::Csa(tex0), batch.state.env.texa);
    for (uint32_t y = 0; y < th; ++y) {
      for (uint32_t x = 0; x < tw; ++x) {
        const uint32_t idx = local.readPixel(view, x, y) & 0xFFu;
        out[static_cast<std::size_t>(y) * tw + x] = (idx < pal.count) ? pal.entries[idx] : 0;
      }
    }
  } else {
    local.readRect(view, 0, 0, tw, th, out.data());
    if (psm_code == hwgs::psm::kCt24) {
      for (auto &p : out) p |= 0xFF000000u;
    }
  }
  return true;
}

} // namespace

#if defined(__SWITCH__)
// GT3-revisited deko lesson: keep textures GPU-resident and avoid WaitIdle per bind.
// Ring of pitch-linear slots — reclaim only after a queue idle (or unused since last wait).
constexpr int kTexSlots = 16;
constexpr uint32_t kTexSlotBytes = 512u * 512u * 4u; // max 512x512 RGBA8 per slot

struct TexSlot {
  DkImage image{};
  DkImageLayout layout{};
  TexKey key{};
  uint32_t w = 0;
  uint32_t h = 0;
  uint32_t mem_off = 0;
  bool occupied = false;
  bool used_since_wait = false;
};

struct WritebackDk::State {
  DkDevice device = nullptr;
  DkQueue queue = nullptr;
  DkCmdBuf cmdbuf = nullptr;
  DkMemBlock cmd_mem = nullptr;
  DkMemBlock code_mem = nullptr;
  DkMemBlock image_mem = nullptr;
  DkMemBlock data_mem = nullptr;
  DkMemBlock stage_mem = nullptr;
  DkMemBlock desc_mem = nullptr;
  DkMemBlock tex_mem = nullptr;
  DkShader vshader{};
  DkShader fshader{};
  DkImage colour{};
  std::uint8_t *stage_cpu = nullptr;
  std::uint8_t *data_cpu = nullptr;
  std::uint8_t *tex_cpu = nullptr;
  uint32_t stage_bytes = 0;
  uint32_t vert_capacity = 0;
  uint32_t vert_offset = 0; // in vertices
  uint32_t tex_capacity_bytes = 0;
  uint32_t cmd_bytes = 0;
  bool recording = false;
  bool colour_seeded = false;
  bool dirty = false;
  int32_t dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = -1, dirty_y1 = -1;
  memory::BufferView frame_view{0, 0, memory::Psm::Ct32};
  bool has_frame_view = false;
  int bound_slot = -1;
  int next_tex_slot = 0;
  TexSlot tex_slots[kTexSlots]{};
  std::unordered_map<TexKey, std::vector<std::uint32_t>, TexKeyHash> tex_cache;
  std::uint64_t tex_cache_bytes = 0;
};
#endif

bool WritebackDk::init(const DkHandles &handles, std::string &error) {
  shutdown();
#if !defined(__SWITCH__)
  (void)handles;
  error = "WritebackDk requires __SWITCH__";
  return false;
#else
  if (!handles.device || !handles.queue) {
    error = "WritebackDk: missing device/queue";
    return false;
  }
  handles_ = handles;
  state_ = new State();
  State &s = *state_;
  s.device = static_cast<DkDevice>(handles.device);
  s.queue = static_cast<DkQueue>(handles.queue);

  // Own a large command buffer — the shared 64 KiB one from create_device is too small for deferred draws.
  s.cmd_bytes = 1024 * 1024;
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, s.device, s.cmd_bytes);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  s.cmd_mem = dkMemBlockCreate(&mm);
  if (!s.cmd_mem) {
    error = "cmd mem failed";
    shutdown();
    return false;
  }
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, s.device);
  s.cmdbuf = dkCmdBufCreate(&cm);
  if (!s.cmdbuf) {
    error = "cmdbuf failed";
    shutdown();
    return false;
  }
  dkCmdBufAddMemory(s.cmdbuf, s.cmd_mem, 0, s.cmd_bytes);

  const uint32_t code_bytes = 256 * 1024;
  dkMemBlockMakerDefaults(&mm, s.device, code_bytes);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
  s.code_mem = dkMemBlockCreate(&mm);
  if (!s.code_mem) {
    error = "code mem failed";
    shutdown();
    return false;
  }
  uint32_t code_off = 0;
  if (!load_shader(s.code_mem, code_off, gs_draw_vsh_dksh_bytes, gs_draw_vsh_dksh_size, s.vshader, error) ||
      !load_shader(s.code_mem, code_off, gs_draw_fsh_dksh_bytes, gs_draw_fsh_dksh_size, s.fshader, error)) {
    shutdown();
    return false;
  }

  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, s.device);
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = kDkSurface;
  lm.dimensions[1] = kDkSurface;
  DkImageLayout colour_layout;
  dkImageLayoutInitialize(&colour_layout, &lm);
  uint32_t colour_size = static_cast<uint32_t>(dkImageLayoutGetSize(&colour_layout));
  uint32_t colour_align = dkImageLayoutGetAlignment(&colour_layout);
  colour_size = (colour_size + colour_align - 1) & ~(colour_align - 1);

  dkMemBlockMakerDefaults(&mm, s.device, colour_size);
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  s.image_mem = dkMemBlockCreate(&mm);
  if (!s.image_mem) {
    error = "colour image mem failed";
    shutdown();
    return false;
  }
  dkImageInitialize(&s.colour, &colour_layout, s.image_mem, 0);

  s.tex_capacity_bytes = kTexSlots * kTexSlotBytes;
  dkMemBlockMakerDefaults(&mm, s.device, s.tex_capacity_bytes + 0x1000u);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  s.tex_mem = dkMemBlockCreate(&mm);
  if (!s.tex_mem) {
    error = "tex mem failed";
    shutdown();
    return false;
  }
  s.tex_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.tex_mem));
  std::memset(s.tex_cpu, 0xFF, s.tex_capacity_bytes);
  for (int i = 0; i < kTexSlots; ++i) {
    TexSlot &slot = s.tex_slots[i];
    slot.mem_off = static_cast<uint32_t>(i) * kTexSlotBytes;
    slot.w = 1;
    slot.h = 1;
    slot.occupied = false;
    slot.used_since_wait = false;
    dkImageLayoutMakerDefaults(&lm, s.device);
    lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = 1;
    lm.dimensions[1] = 1;
    lm.pitchStride = DK_IMAGE_LINEAR_STRIDE_ALIGNMENT;
    dkImageLayoutInitialize(&slot.layout, &lm);
    dkImageInitialize(&slot.image, &slot.layout, s.tex_mem, slot.mem_off);
    reinterpret_cast<std::uint32_t *>(s.tex_cpu + slot.mem_off)[0] = 0xFFFFFFFFu;
  }
  s.bound_slot = 0;
  s.next_tex_slot = 0;

  const uint32_t vert_bytes = 4u * 1024u * 1024u;
  dkMemBlockMakerDefaults(&mm, s.device, vert_bytes);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  s.data_mem = dkMemBlockCreate(&mm);
  if (!s.data_mem) {
    error = "vertex mem failed";
    shutdown();
    return false;
  }
  s.data_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.data_mem));
  s.vert_capacity = vert_bytes / sizeof(GpuVert);

  s.stage_bytes = static_cast<uint32_t>(kDkSurface) * kDkSurface * 4u;
  dkMemBlockMakerDefaults(&mm, s.device, (s.stage_bytes + 0xFFF) & ~0xFFFu);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  s.stage_mem = dkMemBlockCreate(&mm);
  if (!s.stage_mem) {
    error = "stage mem failed";
    shutdown();
    return false;
  }
  s.stage_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.stage_mem));

  dkMemBlockMakerDefaults(&mm, s.device, 0x1000);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  s.desc_mem = dkMemBlockCreate(&mm);
  if (!s.desc_mem) {
    error = "desc mem failed";
    shutdown();
    return false;
  }

  ready_ = true;
  std::string clear_err;
  clear_colour(0, clear_err);
  std::fprintf(stderr, "[hw-dk] WritebackDk deferred GPU drawer ready (%dx%d)\n", kDkSurface, kDkSurface);
  return true;
#endif
}

void WritebackDk::shutdown() {
  ready_ = false;
#if defined(__SWITCH__)
  if (!state_) {
    handles_ = {};
    stats_ = {};
    return;
  }
  State &s = *state_;
  if (s.queue) dkQueueWaitIdle(s.queue);
  if (s.cmdbuf) dkCmdBufDestroy(s.cmdbuf);
  if (s.cmd_mem) dkMemBlockDestroy(s.cmd_mem);
  if (s.desc_mem) dkMemBlockDestroy(s.desc_mem);
  if (s.stage_mem) dkMemBlockDestroy(s.stage_mem);
  if (s.data_mem) dkMemBlockDestroy(s.data_mem);
  if (s.tex_mem) dkMemBlockDestroy(s.tex_mem);
  if (s.image_mem) dkMemBlockDestroy(s.image_mem);
  if (s.code_mem) dkMemBlockDestroy(s.code_mem);
  delete state_;
  state_ = nullptr;
#endif
  handles_ = {};
  stats_ = {};
}

void WritebackDk::note_cpu_fallback() {
#if defined(__SWITCH__)
  if (!state_) return;
  // CPU wrote SLM behind our back — next GPU batch must re-seed the colour target.
  state_->colour_seeded = false;
  state_->dirty = false;
  state_->dirty_x1 = -1;
  ++stats_.cpu_fallback;
#endif
}

void WritebackDk::invalidate_textures() {
#if defined(__SWITCH__)
  if (!state_) return;
  state_->tex_cache.clear();
  state_->tex_cache_bytes = 0;
  for (int i = 0; i < kTexSlots; ++i) {
    state_->tex_slots[i].occupied = false;
    state_->tex_slots[i].key = {};
  }
  state_->bound_slot = -1;
#endif
}

bool WritebackDk::clear_colour(std::uint32_t rgba8, std::string &error) {
#if !defined(__SWITCH__)
  (void)rgba8;
  error = "not switch";
  return false;
#else
  if (!ready_ || !state_) {
    error = "WritebackDk not ready";
    return false;
  }
  State &s = *state_;
  if (s.recording) {
    DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
    dkQueueSubmitCommands(s.queue, list);
    s.recording = false;
    ++stats_.submissions;
  }
  dkQueueWaitIdle(s.queue);
  s.vert_offset = 0;

  const float r = ((rgba8 >> 0) & 0xFF) / 255.f;
  const float g = ((rgba8 >> 8) & 0xFF) / 255.f;
  const float b = ((rgba8 >> 16) & 0xFF) / 255.f;
  const float a = ((rgba8 >> 24) & 0xFF) / 255.f;
  dkCmdBufClear(s.cmdbuf);
  DkImageView view;
  dkImageViewDefaults(&view, &s.colour);
  dkCmdBufBindRenderTarget(s.cmdbuf, &view, nullptr);
  DkViewport vp{0, 0, (float)kDkSurface, (float)kDkSurface, 0, 1};
  DkScissor sc{0, 0, (uint32_t)kDkSurface, (uint32_t)kDkSurface};
  dkCmdBufSetViewports(s.cmdbuf, 0, &vp, 1);
  dkCmdBufSetScissors(s.cmdbuf, 0, &sc, 1);
  dkCmdBufClearColorFloat(s.cmdbuf, 0, DkColorMask_RGBA, r, g, b, a);
  DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
  dkQueueSubmitCommands(s.queue, list);
  dkQueueWaitIdle(s.queue);
  s.colour_seeded = true;
  s.dirty = false;
  s.dirty_x1 = -1;
  ++stats_.clears;
  ++stats_.submissions;
  return true;
#endif
}

bool WritebackDk::flush_to_slm(memory::LocalMemory &local, std::string &error) {
#if !defined(__SWITCH__)
  (void)local;
  error = "not switch";
  return false;
#else
  if (!ready_ || !state_) return true;
  State &s = *state_;
  auto submit = [&](bool wait) {
    if (s.recording) {
      DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
      dkQueueSubmitCommands(s.queue, list);
      s.recording = false;
      ++stats_.submissions;
    }
    if (wait) {
      dkQueueWaitIdle(s.queue);
      s.vert_offset = 0;
    }
  };
  if (!s.dirty || !s.has_frame_view || s.dirty_x1 < s.dirty_x0) {
    // No readback needed — finish any recorded draws without forcing a full idle unless
    // the vertex ring must reset (handled inside submit only when wait=true).
    submit(false);
    return true;
  }

  submit(true);
  for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].used_since_wait = false;

  const uint32_t x0 = static_cast<uint32_t>(std::max(0, s.dirty_x0));
  const uint32_t y0 = static_cast<uint32_t>(std::max(0, s.dirty_y0));
  const uint32_t x1 = static_cast<uint32_t>(std::min(kDkSurface - 1, s.dirty_x1));
  const uint32_t y1 = static_cast<uint32_t>(std::min(kDkSurface - 1, s.dirty_y1));
  if (x1 < x0 || y1 < y0) {
    s.dirty = false;
    return true;
  }
  const uint32_t rw = x1 - x0 + 1;
  const uint32_t rh = y1 - y0 + 1;
  if (static_cast<std::uint64_t>(rw) * rh * 4u > s.stage_bytes) {
    error = "flush rect too large";
    return false;
  }

  dkCmdBufClear(s.cmdbuf);
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &s.colour);
  DkImageRect rect{x0, y0, 0, rw, rh, 1};
  DkCopyBuf dst{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
  dkCmdBufCopyImageToBuffer(s.cmdbuf, &colour_view, &rect, &dst, 0);
  DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
  dkQueueSubmitCommands(s.queue, list);
  dkQueueWaitIdle(s.queue);
  ++stats_.submissions;

  std::vector<std::uint32_t> words(static_cast<std::size_t>(rw) * rh);
  std::memcpy(words.data(), s.stage_cpu, words.size() * 4u);
  local.writeRect(s.frame_view, x0, y0, rw, rh, words.data());
  stats_.readback_bytes += words.size() * 4u;
  ++stats_.flushes;
  s.dirty = false;
  s.dirty_x1 = -1;
  return true;
#endif
}

bool WritebackDk::draw_batch(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                             std::string &error) {
#if !defined(__SWITCH__)
  (void)batch;
  (void)local;
  (void)clut;
  error = "not switch";
  return false;
#else
  if (!ready_ || !state_) {
    error = "WritebackDk not ready";
    return false;
  }
  if (batch.Empty()) return true;

  State &s = *state_;
  const uint64_t prim = batch.state.effectivePrim;
  const bool tme = bit(prim, hwgs::primbit::kTme);
  const bool fst = bit(prim, hwgs::primbit::kFst);
  const bool abe = bit(prim, hwgs::primbit::kAbe);

  const auto &ctx = batch.state.ctx;
  const uint32_t frame_psm_code = hwgs::frame::Psm(ctx.frame);
  const auto frame_psm = memory::decodePsm(frame_psm_code);
  memory::BufferView frame_view{memory::blockOfPage(hwgs::frame::Fbp(ctx.frame)), hwgs::frame::Fbw(ctx.frame),
                                frame_psm ? *frame_psm : memory::Psm::Ct32};

  auto mark_slots_free = [&]() {
    for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].used_since_wait = false;
  };
  auto submit = [&](bool wait) {
    if (s.recording) {
      DkCmdList list = dkCmdBufFinishList(s.cmdbuf);
      dkQueueSubmitCommands(s.queue, list);
      s.recording = false;
      ++stats_.submissions;
    }
    if (wait) {
      dkQueueWaitIdle(s.queue);
      s.vert_offset = 0;
      mark_slots_free();
    }
  };

  // Target change → flush previous GPU work into SLM first.
  if (s.has_frame_view &&
      (s.frame_view.basePointer != frame_view.basePointer || s.frame_view.bufferWidth != frame_view.bufferWidth ||
       s.frame_view.psm != frame_view.psm)) {
    if (!flush_to_slm(local, error)) return false;
    s.colour_seeded = false;
  }
  s.frame_view = frame_view;
  s.has_frame_view = true;

  int32_t x0 = batch.drawRect.x0;
  int32_t y0 = batch.drawRect.y0;
  int32_t x1 = batch.drawRect.x1;
  int32_t y1 = batch.drawRect.y1;
  if (batch.drawRect.Empty()) {
    x0 = y0 = 0;
    x1 = y1 = -1;
    for (const auto &v : batch.vertices) {
      const int32_t px = v.x4 >> 4;
      const int32_t py = v.y4 >> 4;
      if (x1 < x0) {
        x0 = x1 = px;
        y0 = y1 = py;
      } else {
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
      }
    }
  }
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(kDkSurface - 1, x1);
  y1 = std::min(kDkSurface - 1, y1);
  if (x1 < x0 || y1 < y0) return true;

  // Seed colour target from SLM once per coherent GPU run. Record into the same
  // command buffer as the following draws — no WaitIdle until flush_to_slm.
  if (!s.colour_seeded) {
    const uint32_t rw = static_cast<uint32_t>(x1 - x0 + 1);
    const uint32_t rh = static_cast<uint32_t>(y1 - y0 + 1);
    std::vector<std::uint32_t> seed(static_cast<std::size_t>(rw) * rh);
    local.readRect(frame_view, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), rw, rh, seed.data());
    std::memcpy(s.stage_cpu, seed.data(), seed.size() * 4u);
    if (!s.recording) {
      dkCmdBufClear(s.cmdbuf);
      s.recording = true;
    }
    DkImageView colour_view;
    dkImageViewDefaults(&colour_view, &s.colour);
    DkCopyBuf src{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
    DkImageRect rect{static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), 0, rw, rh, 1};
    dkCmdBufCopyBufferToImage(s.cmdbuf, &src, &colour_view, &rect, 0);
    stats_.upload_bytes += seed.size() * 4u;
    s.colour_seeded = true;
  }

  // Texture: CPU decode cache + GPU slot ring (no WaitIdle on every unique TEX0).
  float tex_wf = 1.f, tex_hf = 1.f;
  auto bind_tex_slot = [&](const TexKey &key, const std::uint32_t *texels, uint32_t tw, uint32_t th) -> bool {
    const uint32_t pitch =
        (tw * 4u + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
    if (tw == 0 || th == 0 || tw > 512 || th > 512 || pitch * th > kTexSlotBytes) {
      error = "bad texture size";
      return false;
    }
    for (int i = 0; i < kTexSlots; ++i) {
      if (s.tex_slots[i].occupied && s.tex_slots[i].key == key && s.tex_slots[i].w == tw &&
          s.tex_slots[i].h == th) {
        s.bound_slot = i;
        s.tex_slots[i].used_since_wait = true;
        return true;
      }
    }
    int slot_i = -1;
    for (int n = 0; n < kTexSlots; ++n) {
      const int i = (s.next_tex_slot + n) % kTexSlots;
      if (!s.tex_slots[i].used_since_wait) {
        slot_i = i;
        break;
      }
    }
    if (slot_i < 0) {
      submit(true);
      slot_i = s.next_tex_slot % kTexSlots;
    }
    s.next_tex_slot = (slot_i + 1) % kTexSlots;
    TexSlot &slot = s.tex_slots[slot_i];
    std::uint8_t *dst = s.tex_cpu + slot.mem_off;
    for (uint32_t y = 0; y < th; ++y) {
      std::memcpy(dst + y * pitch, texels + static_cast<std::size_t>(y) * tw, tw * 4u);
    }
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, s.device);
    lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = tw;
    lm.dimensions[1] = th;
    lm.pitchStride = pitch;
    dkImageLayoutInitialize(&slot.layout, &lm);
    dkImageInitialize(&slot.image, &slot.layout, s.tex_mem, slot.mem_off);
    slot.key = key;
    slot.w = tw;
    slot.h = th;
    slot.occupied = true;
    slot.used_since_wait = true;
    s.bound_slot = slot_i;
    stats_.upload_bytes += static_cast<std::uint64_t>(tw) * th * 4u;
    return true;
  };

  if (tme) {
    TexKey key{batch.state.ctx.tex0, batch.state.env.texa, batch.state.clutGeneration};
    const std::vector<std::uint32_t> *texels = nullptr;
    uint32_t tw = 1, th = 1;
    auto it = s.tex_cache.find(key);
    if (it != s.tex_cache.end()) {
      texels = &it->second;
      tw = 1u << hwgs::tex0::Tw(key.tex0);
      th = 1u << hwgs::tex0::Th(key.tex0);
      ++stats_.tex_cache_hits;
    } else {
      std::vector<std::uint32_t> decoded;
      if (!decode_texture(batch, local, clut, decoded, tw, th, error)) return false;
      constexpr std::uint64_t kCacheCap = 48ull * 1024ull * 1024ull;
      const std::uint64_t add = decoded.size() * 4u;
      while (s.tex_cache_bytes + add > kCacheCap && !s.tex_cache.empty()) {
        auto erase_it = s.tex_cache.begin();
        s.tex_cache_bytes -= erase_it->second.size() * 4u;
        s.tex_cache.erase(erase_it);
      }
      auto placed = s.tex_cache.emplace(key, std::move(decoded));
      s.tex_cache_bytes += placed.first->second.size() * 4u;
      texels = &placed.first->second;
    }
    if (!bind_tex_slot(key, texels->data(), tw, th)) return false;
    tex_wf = static_cast<float>(tw);
    tex_hf = static_cast<float>(th);
  } else {
    static const std::uint32_t kWhite = 0xFFFFFFFFu;
    TexKey white{};
    if (!bind_tex_slot(white, &kWhite, 1, 1)) return false;
  }

  std::vector<GpuVert> verts;
  verts.reserve(batch.vertices.size() * 2);
  expand_prims(batch, fst, tex_wf, tex_hf, verts);
  if (verts.empty()) return true;
  if (s.vert_offset + verts.size() > s.vert_capacity) {
    submit(true);
  }
  if (verts.size() > s.vert_capacity) {
    error = "vertex overflow";
    return false;
  }

  const uint32_t base = s.vert_offset;
  std::memcpy(s.data_cpu + base * sizeof(GpuVert), verts.data(), verts.size() * sizeof(GpuVert));
  s.vert_offset = base + static_cast<uint32_t>(verts.size());

  if (!s.recording) {
    dkCmdBufClear(s.cmdbuf);
    s.recording = true;
  }
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &s.colour);
  dkCmdBufBindRenderTarget(s.cmdbuf, &colour_view, nullptr);
  DkViewport vp{0, 0, (float)kDkSurface, (float)kDkSurface, 0, 1};
  DkScissor sc{static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0 + 1),
               static_cast<uint32_t>(y1 - y0 + 1)};
  dkCmdBufSetViewports(s.cmdbuf, 0, &vp, 1);
  dkCmdBufSetScissors(s.cmdbuf, 0, &sc, 1);

  const DkShader *shaders[] = {&s.vshader, &s.fshader};
  dkCmdBufBindShaders(s.cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  dkCmdBufBindRasterizerState(s.cmdbuf, &rs);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, abe);
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  if (abe) {
    dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
                           DkBlendFactor_InvSrcAlpha);
  }
  dkCmdBufBindBlendState(s.cmdbuf, 0, &bs);
  dkCmdBufBindColorState(s.cmdbuf, &cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  dkCmdBufBindColorWriteState(s.cmdbuf, &cws);

  if (s.bound_slot < 0 || s.bound_slot >= kTexSlots) {
    error = "no texture slot";
    return false;
  }
  DkImageView tex_view;
  dkImageViewDefaults(&tex_view, &s.tex_slots[s.bound_slot].image);
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.minFilter = DkFilter_Nearest;
  sampler.magFilter = DkFilter_Nearest;
  sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
  sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
  sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
  auto *desc = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.desc_mem));
  auto *img_desc = reinterpret_cast<DkImageDescriptor *>(desc);
  auto *smp_desc = reinterpret_cast<DkSamplerDescriptor *>(desc + sizeof(DkImageDescriptor));
  dkImageDescriptorInitialize(img_desc, &tex_view, false, false);
  dkSamplerDescriptorInitialize(smp_desc, &sampler);
  dkCmdBufBindImageDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem), 1);
  dkCmdBufBindSamplerDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem) + sizeof(DkImageDescriptor), 1);
  dkCmdBufBindTexture(s.cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));

  DkVtxAttribState attrs[3] = {
      {0, 0, offsetof(GpuVert, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
  };
  DkVtxBufferState bufstate{sizeof(GpuVert), 0};
  dkCmdBufBindVtxAttribState(s.cmdbuf, attrs, 3);
  dkCmdBufBindVtxBufferState(s.cmdbuf, &bufstate, 1);
  dkCmdBufBindVtxBuffer(s.cmdbuf, 0, dkMemBlockGetGpuAddr(s.data_mem) + base * sizeof(GpuVert),
                        static_cast<uint32_t>(verts.size() * sizeof(GpuVert)));
  dkCmdBufDraw(s.cmdbuf, DkPrimitive_Triangles, static_cast<uint32_t>(verts.size()), 1, 0, 0);

  if (s.dirty_x1 < s.dirty_x0) {
    s.dirty_x0 = x0;
    s.dirty_y0 = y0;
    s.dirty_x1 = x1;
    s.dirty_y1 = y1;
  } else {
    s.dirty_x0 = std::min(s.dirty_x0, x0);
    s.dirty_y0 = std::min(s.dirty_y0, y0);
    s.dirty_x1 = std::max(s.dirty_x1, x1);
    s.dirty_y1 = std::max(s.dirty_y1, y1);
  }
  s.dirty = true;
  stats_.batches++;
  stats_.draws++;
  stats_.vertices += verts.size();
  return true;
#endif
}

} // namespace hwgs::dk
