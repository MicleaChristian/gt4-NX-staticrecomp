#include "writeback_dk.hpp"

#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

const char *const kRejectNames[static_cast<int>(Reject::Count)] = {
    "z_write", "z_test", "alpha_test", "dest_alpha", "blend", "fog", "tex_function", "frame_psm",
    "frame_mask", "prim_type", "wrap", "mip", "texture", "surface", "other"};

struct GpuVert {
  float x, y;
  float r, g, b, a;
  float s, t, q;
  float atst, aref;
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

struct Rect {
  int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive
  bool empty() const { return x1 < x0 || y1 < y0; }
  bool contains(const Rect &o) const { return !empty() && o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1; }
};

Rect unite(const Rect &a, const Rect &b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

Rect intersect(const Rect &a, const Rect &b) {
  return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

bool bit(uint64_t prim, unsigned pos) { return ((prim >> pos) & 1u) != 0; }

// GS ALPHA register: A, B, D select Cs (0), Cd (1) or 0 (2); C selects As (0), Ad (1) or FIX (2).
struct BlendEq {
  uint32_t a, b, c, d, fix;
};
BlendEq blend_eq(uint64_t alpha) {
  return {static_cast<uint32_t>(alpha & 3u), static_cast<uint32_t>((alpha >> 2) & 3u),
          static_cast<uint32_t>((alpha >> 4) & 3u), static_cast<uint32_t>((alpha >> 6) & 3u),
          static_cast<uint32_t>((alpha >> 32) & 0xFFu)};
}

// The GPU blends with ((Cs - Cd) * F + Cd) or (Cs * F + Cd) where F is As/128 (dual-source output)
// or FIX/128 (blend constant). Ad needs the destination alpha byte scaled by 128 and is not supported.
bool blend_supported(const BlendEq &e) {
  return e.a == 0 && e.d == 1 && (e.b == 1 || e.b == 2) && (e.c == 0 || e.c == 2);
}

bool reject(DkGpuStats &stats, Reject why) {
  ++stats.rejects[static_cast<int>(why)];
  return false;
}

// Decision on whether the GPU path reproduces this batch. Pure: reads only the batch.
bool accept_batch(const hwgs::Batch &batch, DkGpuStats &stats) {
  const uint64_t prim = batch.state.effectivePrim;
  const auto &ctx = batch.state.ctx;
  const auto &env = batch.state.env;
  const bool tme = bit(prim, hwgs::primbit::kTme);
  const bool abe = bit(prim, hwgs::primbit::kAbe);

  if (bit(prim, hwgs::primbit::kFge)) return reject(stats, Reject::Fog);
  if (bit(prim, hwgs::primbit::kAa1) || (env.colclamp & 1u) == 0) return reject(stats, Reject::Other);

  const uint32_t frame_psm = hwgs::frame::Psm(ctx.frame);
  if (frame_psm != hwgs::psm::kCt32 && frame_psm != hwgs::psm::kCt24) return reject(stats, Reject::FramePsm);
  if (hwgs::frame::Fbmsk(ctx.frame) != 0) return reject(stats, Reject::FrameMask);

  const bool zmsk = hwgs::zbuf::Zmsk(ctx.zbuf) != 0;
  const bool zte = hwgs::test::Zte(ctx.test) != 0;
  const uint32_t ztst = hwgs::test::Ztst(ctx.test);
  if (zte && ztst != 1u) return reject(stats, Reject::ZTest);
  if (!zmsk) return reject(stats, Reject::ZWrite);

  if (hwgs::test::Ate(ctx.test) && hwgs::test::Atst(ctx.test) != 1u) {
    // With Z masked, KEEP and ZB_ONLY both leave a failing pixel untouched; FB_ONLY writes it
    // (handled as ALWAYS when the batch is set up); RGB_ONLY needs an alpha write mask.
    const uint32_t afail = hwgs::test::Afail(ctx.test);
    if (afail == 3u) return reject(stats, Reject::AlphaTest);
  }
  if (hwgs::test::Date(ctx.test) || (ctx.fba & 1u)) return reject(stats, Reject::DestAlpha);
  if (abe && ((env.pabe & 1u) || !blend_supported(blend_eq(ctx.alpha)))) return reject(stats, Reject::Blend);

  for (const auto &p : batch.prims) {
    if (p.type != hwgs::PrimType::Triangle && p.type != hwgs::PrimType::TriangleStrip &&
        p.type != hwgs::PrimType::TriangleFan && p.type != hwgs::PrimType::Sprite)
      return reject(stats, Reject::PrimType);
  }

  if (tme) {
    const uint64_t tex0 = ctx.tex0;
    if (hwgs::tex0::Tfx(tex0) > 1u) return reject(stats, Reject::TexFunction);
    const uint32_t psm = hwgs::tex0::Psm(tex0);
    const bool known_psm = psm == hwgs::psm::kCt32 || psm == hwgs::psm::kCt24 || psm == hwgs::psm::kCt16 ||
                           psm == hwgs::psm::kCt16S || hwgs::IsIndexedPsm(psm);
    if (!known_psm || !memory::decodePsm(psm)) return reject(stats, Reject::Texture);
    if (hwgs::tex0::Tw(tex0) > 9u || hwgs::tex0::Th(tex0) > 9u) return reject(stats, Reject::Texture);
    const uint32_t wms = hwgs::clampreg::Wms(ctx.clamp);
    const uint32_t wmt = hwgs::clampreg::Wmt(ctx.clamp);
    if (wms > 1u || wmt > 1u) return reject(stats, Reject::Wrap);
    if (hwgs::tex1::Mxl(ctx.tex1) > 0u && hwgs::tex1::Mmin(ctx.tex1) >= 2u) return reject(stats, Reject::Mip);
    if (!bit(prim, hwgs::primbit::kFst)) {
      for (const auto &v : batch.vertices) {
        if (!(v.q > 0.0f) || !std::isfinite(v.q) || !std::isfinite(v.s) || !std::isfinite(v.t))
          return reject(stats, Reject::Other);
      }
    }
  }
  return true;
}

// Texel colour expansion to RGBA8 (alpha byte as stored; 0x80 = 1.0 for the GS).
uint32_t texa_alpha(uint32_t rgb, bool msb, uint64_t texa) {
  const uint32_t ta0 = static_cast<uint32_t>(texa & 0xFFu);
  const bool aem = ((texa >> 15) & 1u) != 0;
  const uint32_t ta1 = static_cast<uint32_t>((texa >> 32) & 0xFFu);
  if (msb) return ta1;
  return (aem && rgb == 0) ? 0u : ta0;
}

bool decode_texture(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                    std::vector<std::uint32_t> &out, uint32_t &tw, uint32_t &th) {
  const uint64_t tex0 = batch.state.ctx.tex0;
  const uint64_t texa = batch.state.env.texa;
  const uint32_t psm_code = hwgs::tex0::Psm(tex0);
  const auto psm = memory::decodePsm(psm_code);
  if (!psm) return false;
  tw = 1u << hwgs::tex0::Tw(tex0);
  th = 1u << hwgs::tex0::Th(tex0);
  memory::BufferView view{hwgs::tex0::Tbp0(tex0), hwgs::tex0::Tbw(tex0), *psm};
  out.resize(static_cast<std::size_t>(tw) * th);
  if (hwgs::IsIndexedPsm(psm_code)) {
    const auto pal = clut.Decode(psm_code, hwgs::tex0::Cpsm(tex0), hwgs::tex0::Csa(tex0), texa);
    for (uint32_t y = 0; y < th; ++y) {
      for (uint32_t x = 0; x < tw; ++x) {
        const uint32_t idx = local.readPixel(view, x, y) & 0xFFu;
        out[static_cast<std::size_t>(y) * tw + x] = (idx < pal.count) ? pal.entries[idx] : 0;
      }
    }
  } else {
    local.readRect(view, 0, 0, tw, th, out.data());
    if (psm_code == hwgs::psm::kCt24) {
      for (auto &p : out) {
        const uint32_t rgb = p & 0x00FFFFFFu;
        p = rgb | (texa_alpha(rgb, false, texa) << 24);
      }
    } else if (psm_code == hwgs::psm::kCt16 || psm_code == hwgs::psm::kCt16S) {
      for (auto &p : out) {
        const uint32_t r = (p & 0x1Fu) << 3, g = ((p >> 5) & 0x1Fu) << 3, b = ((p >> 10) & 0x1Fu) << 3;
        const uint32_t rgb = r | (g << 8) | (b << 16);
        p = rgb | (texa_alpha(rgb, (p & 0x8000u) != 0, texa) << 24);
      }
    }
  }
  if (hwgs::tex0::Tcc(tex0) == 0) {
    // TCC = 0: the fragment alpha is the vertex alpha. A texel alpha of 0x80 (1.0) makes the
    // shader's multiply return it unchanged.
    for (auto &p : out) p = (p & 0x00FFFFFFu) | 0x80000000u;
  }
  return true;
}

// GS samples a pixel at its integer coordinate; the GPU at the pixel centre, hence the half-pixel shift.
float ndc_x(float px) { return (px + 0.5f) / static_cast<float>(kDkSurface) * 2.f - 1.f; }
float ndc_y(float py) { return -((py + 0.5f) / static_cast<float>(kDkSurface) * 2.f - 1.f); }

struct VertexSetup {
  bool tme = false, fst = false, flat = false;
  float tex_w = 1.f, tex_h = 1.f;
  float colour_scale = 1.f / 255.f; // rgb scale
  float alpha_scale = 1.f / 255.f;
  bool rgb_one = false;             // decal: texture colour replaces the vertex colour
  float atst = 1.f, aref = 0.f;
};

GpuVert make_vert(const hwgs::Vertex &v, const hwgs::Vertex &colour_src, const VertexSetup &vs) {
  GpuVert o{};
  o.x = ndc_x(static_cast<float>(v.x4) / 16.f);
  o.y = ndc_y(static_cast<float>(v.y4) / 16.f);
  const hwgs::Vertex &c = vs.flat ? colour_src : v;
  if (vs.rgb_one) {
    o.r = o.g = o.b = 1.f;
  } else {
    o.r = c.r * vs.colour_scale;
    o.g = c.g * vs.colour_scale;
    o.b = c.b * vs.colour_scale;
  }
  o.a = c.a * vs.alpha_scale;
  if (!vs.tme) {
    o.s = o.t = 0.5f;
    o.q = 1.f;
  } else if (vs.fst) {
    o.s = (static_cast<float>(v.u) / 16.f) / vs.tex_w;
    o.t = (static_cast<float>(v.v) / 16.f) / vs.tex_h;
    o.q = 1.f;
  } else {
    o.s = v.s;
    o.t = v.t;
    o.q = v.q;
  }
  o.atst = vs.atst;
  o.aref = vs.aref;
  return o;
}

void expand_prims(const hwgs::Batch &batch, const VertexSetup &vs, std::vector<GpuVert> &verts) {
  for (const auto &prim : batch.prims) {
    if (prim.first + prim.count > batch.vertices.size()) continue;
    const auto *pv = batch.vertices.data() + prim.first;
    switch (prim.type) {
    case hwgs::PrimType::Triangle:
      if (prim.count >= 3) {
        verts.push_back(make_vert(pv[0], pv[2], vs));
        verts.push_back(make_vert(pv[1], pv[2], vs));
        verts.push_back(make_vert(pv[2], pv[2], vs));
      }
      break;
    case hwgs::PrimType::TriangleStrip:
    case hwgs::PrimType::TriangleFan:
      // The front end records strips and fans as one primitive per kicked triangle.
      if (prim.count >= 3) {
        verts.push_back(make_vert(pv[0], pv[2], vs));
        verts.push_back(make_vert(pv[1], pv[2], vs));
        verts.push_back(make_vert(pv[2], pv[2], vs));
      }
      break;
    case hwgs::PrimType::Sprite:
      if (prim.count >= 2) {
        // Sprites are always flat-shaded with the second vertex colour.
        VertexSetup flat = vs;
        flat.flat = true;
        const GpuVert a = make_vert(pv[0], pv[1], flat);
        const GpuVert b = make_vert(pv[1], pv[1], flat);
        GpuVert v01 = a, v10 = a;
        v01.y = b.y;
        v01.t = b.t;
        v10.x = b.x;
        v10.s = b.s;
        verts.push_back(a);
        verts.push_back(v10);
        verts.push_back(b);
        verts.push_back(a);
        verts.push_back(b);
        verts.push_back(v01);
      }
      break;
    default:
      break;
    }
  }
}

} // namespace

#if defined(__SWITCH__)
namespace {

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

// Keep textures GPU-resident: a ring of pitch-linear slots, each with its own image descriptor so
// draws recorded earlier in the same command list keep sampling their own texture.
constexpr int kTexSlots = 16;
constexpr uint32_t kTexSlotBytes = 512u * 512u * 4u; // max 512x512 RGBA8 per slot
constexpr int kWhiteSlot = kTexSlots;                // 1x1 white for untextured batches
constexpr int kImageDescriptors = kTexSlots + 1;
// Sampler index = filter (0 nearest, 1 linear) * 4 + wrap S (0 repeat, 1 clamp) * 2 + wrap T.
constexpr int kSamplers = 8;

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

} // namespace

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
  DkImageDescriptor *image_desc = nullptr;
  DkSamplerDescriptor *sampler_desc = nullptr;
  uint32_t sampler_desc_offset = 0;
  uint32_t stage_bytes = 0;
  uint32_t vert_capacity = 0;
  uint32_t vert_offset = 0; // in vertices
  uint32_t cmd_bytes = 0;
  bool recording = false;
  bool descriptors_bound = false;
  Rect seeded;  // surface area whose GPU copy equals SLM (or holds newer pixels inside `dirty`)
  Rect dirty;   // surface area drawn on the GPU and not yet written back
  memory::BufferView frame_view{0, 0, memory::Psm::Ct32};
  uint32_t frame_fbw = 0;
  bool has_frame_view = false;
  int next_tex_slot = 0;
  TexSlot tex_slots[kTexSlots]{};
  TexSlot white{};
  std::unordered_map<TexKey, std::vector<std::uint32_t>, TexKeyHash> tex_cache;
  std::uint64_t tex_cache_bytes = 0;

  void begin_recording() {
    if (!recording) {
      dkCmdBufClear(cmdbuf);
      recording = true;
      descriptors_bound = false;
    }
  }
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

  const uint32_t tex_bytes = kTexSlots * kTexSlotBytes + 0x1000u; // last page: the white texel
  dkMemBlockMakerDefaults(&mm, s.device, tex_bytes);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  s.tex_mem = dkMemBlockCreate(&mm);
  if (!s.tex_mem) {
    error = "tex mem failed";
    shutdown();
    return false;
  }
  s.tex_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.tex_mem));
  std::memset(s.tex_cpu, 0, tex_bytes);
  for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].mem_off = static_cast<uint32_t>(i) * kTexSlotBytes;
  s.white.mem_off = kTexSlots * kTexSlotBytes;
  reinterpret_cast<std::uint32_t *>(s.tex_cpu + s.white.mem_off)[0] = 0xFFFFFFFFu;
  dkImageLayoutMakerDefaults(&lm, s.device);
  lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = 1;
  lm.dimensions[1] = 1;
  lm.pitchStride = DK_IMAGE_LINEAR_STRIDE_ALIGNMENT;
  dkImageLayoutInitialize(&s.white.layout, &lm);
  dkImageInitialize(&s.white.image, &s.white.layout, s.tex_mem, s.white.mem_off);

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
  auto *desc = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.desc_mem));
  s.image_desc = reinterpret_cast<DkImageDescriptor *>(desc);
  s.sampler_desc_offset = static_cast<uint32_t>(kImageDescriptors * sizeof(DkImageDescriptor));
  s.sampler_desc_offset = (s.sampler_desc_offset + DK_SAMPLER_DESCRIPTOR_ALIGNMENT - 1) &
                          ~(DK_SAMPLER_DESCRIPTOR_ALIGNMENT - 1);
  s.sampler_desc = reinterpret_cast<DkSamplerDescriptor *>(desc + s.sampler_desc_offset);
  {
    DkImageView white_view;
    dkImageViewDefaults(&white_view, &s.white.image);
    dkImageDescriptorInitialize(&s.image_desc[kWhiteSlot], &white_view, false, false);
  }
  for (int i = 0; i < kSamplers; ++i) {
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    const bool linear = (i & 4) != 0;
    sampler.minFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
    sampler.magFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
    sampler.wrapMode[0] = (i & 2) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
    sampler.wrapMode[1] = (i & 1) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
    sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize(&s.sampler_desc[i], &sampler);
  }

  ready_ = true;
  std::string clear_err;
  clear_colour(0, clear_err);
  std::fprintf(stderr, "[hw-dk] WritebackDk GPU drawer ready (%dx%d, exact-subset acceptance)\n", kDkSurface,
               kDkSurface);
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

void WritebackDk::report(std::FILE *out) const {
  if (!out) return;
  std::fprintf(out,
               "[hw-dk] GPU batches=%llu cpu_fallback=%llu seeds=%llu flushes=%llu readback_MiB=%.1f "
               "upload_MiB=%.1f\n",
               static_cast<unsigned long long>(stats_.batches), static_cast<unsigned long long>(stats_.cpu_fallback),
               static_cast<unsigned long long>(stats_.seeds), static_cast<unsigned long long>(stats_.flushes),
               stats_.readback_bytes / 1048576.0, stats_.upload_bytes / 1048576.0);
  std::fprintf(out, "[hw-dk] CPU fallback reasons:");
  for (int i = 0; i < static_cast<int>(Reject::Count); ++i) {
    if (stats_.rejects[i]) std::fprintf(out, " %s=%llu", kRejectNames[i], static_cast<unsigned long long>(stats_.rejects[i]));
  }
  std::fprintf(out, "\n");
}

void WritebackDk::memory_written() {
#if defined(__SWITCH__)
  if (!state_) return;
  state_->seeded = {};
  invalidate_textures();
#endif
}

void WritebackDk::note_cpu_fallback() {
#if defined(__SWITCH__)
  if (!state_) return;
  memory_written();
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
#endif
}

void WritebackDk::before_memory_access(memory::LocalMemory &local, std::uint32_t block, std::uint32_t blocks,
                                       bool write) {
#if !defined(__SWITCH__)
  (void)local;
  (void)block;
  (void)blocks;
  (void)write;
#else
  if (!ready_ || !state_) return;
  State &s = *state_;
  if (!s.has_frame_view) {
    if (write) invalidate_textures();
    return;
  }
  const Rect held = unite(s.seeded, s.dirty);
  const bool overlaps =
      !held.empty() && hwgs::BlockRangesOverlap(block, blocks, s.frame_view.basePointer,
                                                hwgs::FootprintBlocks(s.frame_fbw, static_cast<uint32_t>(held.y1 + 1)));
  if (overlaps && !s.dirty.empty()) {
    std::string err;
    flush_to_slm(local, err);
  }
  if (write) {
    if (overlaps) s.seeded = {};
    invalidate_textures();
  }
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
  s.seeded = {};
  s.dirty = {};
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
  if (s.dirty.empty() || !s.has_frame_view) {
    if (s.recording) {
      dkQueueSubmitCommands(s.queue, dkCmdBufFinishList(s.cmdbuf));
      s.recording = false;
      ++stats_.submissions;
    }
    return true;
  }

  const Rect r = intersect(s.dirty, Rect{0, 0, kDkSurface - 1, kDkSurface - 1});
  s.dirty = {};
  if (r.empty()) return true;
  const uint32_t rw = static_cast<uint32_t>(r.x1 - r.x0 + 1);
  const uint32_t rh = static_cast<uint32_t>(r.y1 - r.y0 + 1);
  if (static_cast<std::uint64_t>(rw) * rh * 4u > s.stage_bytes) {
    error = "flush rect too large";
    return false;
  }

  // The readback goes in the same command list as the draws, then one wait.
  s.begin_recording();
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &s.colour);
  DkImageRect rect{static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), 0, rw, rh, 1};
  DkCopyBuf dst{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
  dkCmdBufCopyImageToBuffer(s.cmdbuf, &colour_view, &rect, &dst, 0);
  dkQueueSubmitCommands(s.queue, dkCmdBufFinishList(s.cmdbuf));
  s.recording = false;
  ++stats_.submissions;
  dkQueueWaitIdle(s.queue);
  s.vert_offset = 0;
  for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].used_since_wait = false;

  local.writeRect(s.frame_view, static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), rw, rh,
                  reinterpret_cast<const std::uint32_t *>(s.stage_cpu));
  stats_.readback_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
  ++stats_.flushes;
  // Pixels just written back may be sampled as a texture later.
  invalidate_textures();
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
  if (!accept_batch(batch, stats_)) return false;

  State &s = *state_;
  const auto &ctx = batch.state.ctx;
  const uint64_t prim = batch.state.effectivePrim;
  const bool tme = bit(prim, hwgs::primbit::kTme);
  const bool abe = bit(prim, hwgs::primbit::kAbe);

  // Pixel area: scissor (inclusive) intersected with the batch coverage.
  const Rect scissor{static_cast<int32_t>(BitRange(ctx.scissor, 0, 10)), static_cast<int32_t>(BitRange(ctx.scissor, 32, 42)),
                     static_cast<int32_t>(BitRange(ctx.scissor, 16, 26)), static_cast<int32_t>(BitRange(ctx.scissor, 48, 58))};
  Rect area{batch.drawRect.x0, batch.drawRect.y0, batch.drawRect.x1, batch.drawRect.y1};
  if (area.empty()) {
    for (const auto &v : batch.vertices) {
      const int32_t px = v.x4 >> 4, py = v.y4 >> 4;
      area = unite(area, Rect{px, py, px + 1, py + 1});
    }
  }
  area = intersect(area, scissor);
  if (area.empty()) return true;
  if (area.x0 < 0 || area.y0 < 0 || area.x1 >= kDkSurface || area.y1 >= kDkSurface) {
    return reject(stats_, Reject::Surface);
  }

  const uint32_t frame_psm_code = hwgs::frame::Psm(ctx.frame);
  const memory::BufferView frame_view{memory::blockOfPage(hwgs::frame::Fbp(ctx.frame)), hwgs::frame::Fbw(ctx.frame),
                                      *memory::decodePsm(frame_psm_code)};

  auto submit_and_wait = [&]() {
    if (s.recording) {
      dkQueueSubmitCommands(s.queue, dkCmdBufFinishList(s.cmdbuf));
      s.recording = false;
      ++stats_.submissions;
    }
    dkQueueWaitIdle(s.queue);
    s.vert_offset = 0;
    for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].used_since_wait = false;
  };

  // New render target: write the old one back and start a fresh GPU copy.
  if (s.has_frame_view && (s.frame_view.basePointer != frame_view.basePointer ||
                           s.frame_view.bufferWidth != frame_view.bufferWidth || s.frame_view.psm != frame_view.psm)) {
    if (!flush_to_slm(local, error)) return false;
    s.seeded = {};
  }
  s.frame_view = frame_view;
  s.frame_fbw = hwgs::frame::Fbw(ctx.frame);
  s.has_frame_view = true;

  // A texture read from the area the GPU holds newer pixels for needs them in SLM first.
  if (tme && !s.dirty.empty()) {
    const uint64_t tex0 = ctx.tex0;
    const uint32_t tex_blocks = hwgs::FootprintBlocks(hwgs::tex0::Tbw(tex0), 1u << hwgs::tex0::Th(tex0));
    const uint32_t frame_blocks = hwgs::FootprintBlocks(s.frame_fbw, static_cast<uint32_t>(s.dirty.y1 + 1));
    if (hwgs::BlockRangesOverlap(hwgs::tex0::Tbp0(tex0), tex_blocks, frame_view.basePointer, frame_blocks)) {
      if (!flush_to_slm(local, error)) return false;
    }
  }

  // Grow the GPU copy to cover this batch. SLM must hold every GPU pixel first.
  if (!s.seeded.contains(area)) {
    if (!s.dirty.empty() && !flush_to_slm(local, error)) return false;
    const Rect seed = unite(s.seeded, area);
    const uint32_t rw = static_cast<uint32_t>(seed.x1 - seed.x0 + 1);
    const uint32_t rh = static_cast<uint32_t>(seed.y1 - seed.y0 + 1);
    // The staging buffer may still feed a recorded copy; wait before overwriting it.
    submit_and_wait();
    local.readRect(frame_view, static_cast<uint32_t>(seed.x0), static_cast<uint32_t>(seed.y0), rw, rh,
                   reinterpret_cast<std::uint32_t *>(s.stage_cpu));
    s.begin_recording();
    DkImageView colour_view;
    dkImageViewDefaults(&colour_view, &s.colour);
    DkCopyBuf src{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
    DkImageRect rect{static_cast<uint32_t>(seed.x0), static_cast<uint32_t>(seed.y0), 0, rw, rh, 1};
    dkCmdBufCopyBufferToImage(s.cmdbuf, &src, &colour_view, &rect, 0);
    stats_.upload_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
    ++stats_.seeds;
    s.seeded = seed;
  }

  // Texture: decoded once per (TEX0, TEXA, CLUT) until local memory changes, uploaded into a slot.
  int image_index = kWhiteSlot;
  VertexSetup vs;
  vs.tme = tme;
  vs.fst = bit(prim, hwgs::primbit::kFst);
  vs.flat = !bit(prim, hwgs::primbit::kIip);
  bool linear = false;
  bool clamp_s = true, clamp_t = true;
  if (tme) {
    const TexKey key{ctx.tex0, batch.state.env.texa, batch.state.clutGeneration};
    uint32_t tw = 1u << hwgs::tex0::Tw(key.tex0);
    uint32_t th = 1u << hwgs::tex0::Th(key.tex0);
    const std::vector<std::uint32_t> *texels = nullptr;
    auto it = s.tex_cache.find(key);
    if (it != s.tex_cache.end()) {
      texels = &it->second;
      ++stats_.tex_cache_hits;
    } else {
      std::vector<std::uint32_t> decoded;
      if (!decode_texture(batch, local, clut, decoded, tw, th)) return reject(stats_, Reject::Texture);
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

    int slot_i = -1;
    for (int i = 0; i < kTexSlots; ++i) {
      const TexSlot &t = s.tex_slots[i];
      if (t.occupied && t.key == key && t.w == tw && t.h == th) {
        slot_i = i;
        break;
      }
    }
    if (slot_i < 0) {
      for (int n = 0; n < kTexSlots && slot_i < 0; ++n) {
        const int i = (s.next_tex_slot + n) % kTexSlots;
        if (!s.tex_slots[i].used_since_wait) slot_i = i;
      }
      if (slot_i < 0) {
        // Every slot feeds a recorded draw: run them before reusing one.
        submit_and_wait();
        slot_i = s.next_tex_slot % kTexSlots;
      }
      s.next_tex_slot = (slot_i + 1) % kTexSlots;
      TexSlot &slot = s.tex_slots[slot_i];
      const uint32_t pitch =
          (tw * 4u + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
      if (static_cast<std::uint64_t>(pitch) * th > kTexSlotBytes) return reject(stats_, Reject::Texture);
      std::uint8_t *dst = s.tex_cpu + slot.mem_off;
      for (uint32_t y = 0; y < th; ++y) std::memcpy(dst + y * pitch, texels->data() + static_cast<std::size_t>(y) * tw, tw * 4u);
      DkImageLayoutMaker lm;
      dkImageLayoutMakerDefaults(&lm, s.device);
      lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
      lm.format = DkImageFormat_RGBA8_Unorm;
      lm.dimensions[0] = tw;
      lm.dimensions[1] = th;
      lm.pitchStride = pitch;
      dkImageLayoutInitialize(&slot.layout, &lm);
      dkImageInitialize(&slot.image, &slot.layout, s.tex_mem, slot.mem_off);
      DkImageView view;
      dkImageViewDefaults(&view, &slot.image);
      dkImageDescriptorInitialize(&s.image_desc[slot_i], &view, false, false);
      slot.key = key;
      slot.w = tw;
      slot.h = th;
      slot.occupied = true;
      stats_.upload_bytes += static_cast<std::uint64_t>(tw) * th * 4u;
    }
    s.tex_slots[slot_i].used_since_wait = true;
    image_index = slot_i;

    vs.tex_w = static_cast<float>(tw);
    vs.tex_h = static_cast<float>(th);
    // Modulate: colour = Ct * Cv / 128 (0x80 = 1.0). Decal: colour = Ct. With TCC = 0 the decoded texel
    // alpha is 0x80, so the shader's product leaves the vertex alpha.
    vs.colour_scale = 1.f / 128.f;
    vs.alpha_scale = 1.f / 128.f;
    vs.rgb_one = hwgs::tex0::Tfx(ctx.tex0) == 1u;
    linear = hwgs::tex1::Mmag(ctx.tex1) != 0u;
    clamp_s = hwgs::clampreg::Wms(ctx.clamp) == 1u;
    clamp_t = hwgs::clampreg::Wmt(ctx.clamp) == 1u;
  }

  if (hwgs::test::Ate(ctx.test)) {
    const uint32_t atst = hwgs::test::Atst(ctx.test);
    const uint32_t afail = hwgs::test::Afail(ctx.test);
    // FB_ONLY with Z masked writes the pixel whatever the result.
    vs.atst = afail == 1u ? 1.f : static_cast<float>(atst);
    vs.aref = static_cast<float>(hwgs::test::Aref(ctx.test));
  }

  std::vector<GpuVert> verts;
  verts.reserve(batch.vertices.size() * 2);
  expand_prims(batch, vs, verts);
  if (vs.rgb_one && hwgs::tex0::Tcc(ctx.tex0)) {
    // Decal with texture alpha: the texel alpha passes through unscaled.
    for (auto &v : verts) v.a = 1.f;
  }
  if (verts.empty()) return true;
  if (verts.size() > s.vert_capacity) return reject(stats_, Reject::Other);
  if (s.vert_offset + verts.size() > s.vert_capacity) submit_and_wait();

  const uint32_t base = s.vert_offset;
  std::memcpy(s.data_cpu + base * sizeof(GpuVert), verts.data(), verts.size() * sizeof(GpuVert));
  s.vert_offset = base + static_cast<uint32_t>(verts.size());

  s.begin_recording();
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &s.colour);
  dkCmdBufBindRenderTarget(s.cmdbuf, &colour_view, nullptr);
  DkViewport vp{0, 0, (float)kDkSurface, (float)kDkSurface, 0, 1};
  DkScissor sc{static_cast<uint32_t>(area.x0), static_cast<uint32_t>(area.y0), static_cast<uint32_t>(area.x1 - area.x0 + 1),
               static_cast<uint32_t>(area.y1 - area.y0 + 1)};
  dkCmdBufSetViewports(s.cmdbuf, 0, &vp, 1);
  dkCmdBufSetScissors(s.cmdbuf, 0, &sc, 1);

  const DkShader *shaders[] = {&s.vshader, &s.fshader};
  dkCmdBufBindShaders(s.cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(s.cmdbuf, &rs);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, abe);
  dkCmdBufBindColorState(s.cmdbuf, &cs);
  if (abe) {
    const BlendEq eq = blend_eq(ctx.alpha);
    const bool fixed = eq.c == 2u;
    const DkBlendFactor src = fixed ? DkBlendFactor_ConstAlpha : DkBlendFactor_Src1Alpha;
    const DkBlendFactor dst =
        eq.b == 1u ? (fixed ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvSrc1Alpha) : DkBlendFactor_One;
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkBlendStateSetFactors(&bs, src, dst, DkBlendFactor_One, DkBlendFactor_Zero);
    dkCmdBufBindBlendState(s.cmdbuf, 0, &bs);
    if (fixed) dkCmdBufSetBlendConst(s.cmdbuf, 0.f, 0.f, 0.f, std::min(1.f, eq.fix / 128.f));
  }
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  dkCmdBufBindColorWriteState(s.cmdbuf, &cws);

  if (!s.descriptors_bound) {
    dkCmdBufBindImageDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem), kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem) + s.sampler_desc_offset, kSamplers);
    s.descriptors_bound = true;
  }
  const int sampler_index = (linear ? 4 : 0) + (clamp_s ? 2 : 0) + (clamp_t ? 1 : 0);
  dkCmdBufBindTexture(s.cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(image_index, sampler_index));

  DkVtxAttribState attrs[4] = {
      {0, 0, offsetof(GpuVert, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, atst), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
  };
  DkVtxBufferState bufstate{sizeof(GpuVert), 0};
  dkCmdBufBindVtxAttribState(s.cmdbuf, attrs, 4);
  dkCmdBufBindVtxBufferState(s.cmdbuf, &bufstate, 1);
  dkCmdBufBindVtxBuffer(s.cmdbuf, 0, dkMemBlockGetGpuAddr(s.data_mem) + base * sizeof(GpuVert),
                        static_cast<uint32_t>(verts.size() * sizeof(GpuVert)));
  dkCmdBufDraw(s.cmdbuf, DkPrimitive_Triangles, static_cast<uint32_t>(verts.size()), 1, 0, 0);

  s.dirty = unite(s.dirty, area);
  stats_.batches++;
  stats_.draws++;
  stats_.vertices += verts.size();
  return true;
#endif
}

} // namespace hwgs::dk
