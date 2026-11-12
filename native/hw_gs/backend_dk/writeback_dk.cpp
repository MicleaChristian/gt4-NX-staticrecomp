#include "writeback_dk.hpp"

#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__SWITCH__)
#include <deko3d.h>
#include <malloc.h> // memalign

#include "gs_draw_fsh_dksh.hpp"
#include "gs_draw_vsh_dksh.hpp"
#endif

namespace hwgs::dk {
namespace {

const char *const kRejectNames[static_cast<int>(Reject::Count)] = {
    "depth_unavailable", "z_format", "alpha_fail_blend", "dest_alpha_test", "blend", "colclamp",
    "frame_psm", "frame_mask", "prim_mix", "texture", "surface", "other"};

struct GpuVert {
  float x, y, z;
  float r, g, b, a;
  float s, t, q;
  float fog;
};

// std140 layout of the fragment shader's Params block.
struct Params {
  float test[4];
  float texfn[4];
  float fogcol[4];
  float wrap[4];
  float region[4];
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

constexpr uint32_t kAtstAlways = 1;
constexpr uint32_t kAfailKeep = 0, kAfailFbOnly = 1, kAfailZbOnly = 2, kAfailRgbOnly = 3;
constexpr uint32_t kZtstNever = 0, kZtstAlways = 1, kZtstGequal = 2;

// Z buffer formats: ZBUF.PSM holds the low four bits of the Z PSM code.
uint32_t z_psm_code(uint64_t zbuf) { return 0x30u | hwgs::zbuf::Psm(zbuf); }
bool z_format_known(uint32_t code) {
  return code == hwgs::psm::kZ32 || code == hwgs::psm::kZ24 || code == hwgs::psm::kZ16 || code == hwgs::psm::kZ16S;
}
uint32_t z_max(uint32_t code) {
  if (code == hwgs::psm::kZ32) return 0xFFFFFFFFu;
  if (code == hwgs::psm::kZ24) return 0x00FFFFFFu;
  return 0xFFFFu;
}
// Depth in [0, 1) as z / 2^bits; Z24 and Z16 values are exact in a 32-bit float, Z32 keeps 24 bits.
double z_scale(uint32_t code) {
  if (code == hwgs::psm::kZ32) return 1.0 / 4294967296.0;
  if (code == hwgs::psm::kZ24) return 1.0 / 16777216.0;
  return 1.0 / 65536.0;
}

// Batch-level depth facts.
struct DepthUse {
  bool test = false;   // depth test other than ALWAYS
  bool write = false;  // ZMSK = 0
  bool never = false;  // ZTST = NEVER: nothing is drawn
  bool any() const { return test || write; }
};
DepthUse depth_use(const hwgs::ContextRegs &ctx) {
  DepthUse d;
  const bool zte = hwgs::test::Zte(ctx.test) != 0;
  const uint32_t ztst = hwgs::test::Ztst(ctx.test);
  d.never = zte && ztst == kZtstNever;
  d.test = zte && ztst != kZtstAlways && ztst != kZtstNever;
  d.write = hwgs::zbuf::Zmsk(ctx.zbuf) == 0;
  return d;
}

// Alpha test fail handling. A fail action that writes something needs a second pass over the
// failing pixels with its own write masks.
struct AlphaPlan {
  uint32_t atst = kAtstAlways;
  float aref = 0.f;
  bool second_pass = false;
  bool second_colour = false;
  bool second_alpha = false;
  bool second_depth = false;
};
AlphaPlan alpha_plan(const hwgs::ContextRegs &ctx, bool zwrite) {
  AlphaPlan p;
  if (!hwgs::test::Ate(ctx.test) || hwgs::test::Atst(ctx.test) == kAtstAlways) return p;
  p.atst = hwgs::test::Atst(ctx.test);
  p.aref = static_cast<float>(hwgs::test::Aref(ctx.test));
  switch (hwgs::test::Afail(ctx.test)) {
  case kAfailKeep:
    break;
  case kAfailFbOnly:
    if (!zwrite) {
      p.atst = kAtstAlways; // colour written either way, nothing else differs
    } else {
      p.second_pass = p.second_colour = p.second_alpha = true;
    }
    break;
  case kAfailZbOnly:
    if (zwrite) p.second_pass = p.second_depth = true;
    break;
  case kAfailRgbOnly:
  default:
    p.second_pass = p.second_colour = true;
    break;
  }
  return p;
}

// GS blend: ((A - B) * C >> 7) + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Expanded as
// Cs * ks + Cd * kd with k = c0 + cf * F; the GPU computes src * Fs (op) dst * Fd.
struct Coef {
  int c0 = 0;
  int cf = 0;
};
struct GpuBlend {
  bool enable = false;
#if defined(__SWITCH__)
  DkBlendFactor src = DkBlendFactor_One;
  DkBlendFactor dst = DkBlendFactor_Zero;
  DkBlendOp op = DkBlendOp_Add;
#endif
  float constant = 0.f;
};

bool map_blend(uint64_t alpha, GpuBlend &out) {
  const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
  const uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFFu);
  auto coef = [&](uint32_t which) {
    return Coef{d == which ? 1 : 0, (a == which ? 1 : 0) - (b == which ? 1 : 0)};
  };
  const Coef ks = coef(0), kd = coef(1);
  if ((ks.cf != 0 || kd.cf != 0) && c != 0 && c != 2) return false; // Ad coefficient (or reserved)
  const bool fixed = c == 2;
  out.constant = std::min(1.f, fix / 128.f);
#if defined(__SWITCH__)
  auto factor = [&](const Coef &k, DkBlendFactor &f) {
    if (k.c0 == 0 && k.cf == 0) f = DkBlendFactor_Zero;
    else if (k.c0 == 1 && k.cf == 0) f = DkBlendFactor_One;
    else if (k.c0 == 0 && k.cf == 1) f = fixed ? DkBlendFactor_ConstAlpha : DkBlendFactor_Src1Alpha;
    else if (k.c0 == 1 && k.cf == -1) f = fixed ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvSrc1Alpha;
    else return false;
    return true;
  };
  auto negative_f = [](const Coef &k) { return k.c0 == 0 && k.cf == -1; };
  const DkBlendFactor f_pos = fixed ? DkBlendFactor_ConstAlpha : DkBlendFactor_Src1Alpha;
  DkBlendFactor fs{}, fd{};
  if (factor(ks, fs) && factor(kd, fd)) {
    out.op = DkBlendOp_Add;
  } else if (negative_f(ks) && factor(kd, fd)) {
    out.op = DkBlendOp_RevSub; // dst * Fd - src * F
    fs = f_pos;
  } else if (negative_f(kd) && factor(ks, fs)) {
    out.op = DkBlendOp_Sub; // src * Fs - dst * F
    fd = f_pos;
  } else if (negative_f(ks) && negative_f(kd)) {
    out.op = DkBlendOp_Add; // -(Cs + Cd) * F clamps to zero
    fs = fd = DkBlendFactor_Zero;
  } else {
    return false;
  }
  out.src = fs;
  out.dst = fd;
  out.enable = !(out.op == DkBlendOp_Add && fs == DkBlendFactor_One && fd == DkBlendFactor_Zero);
#else
  (void)ks;
  (void)kd;
  (void)fixed;
#endif
  return true;
}

bool reject(DkGpuStats &stats, Reject why) {
  ++stats.rejects[static_cast<int>(why)];
  return false;
}

// Decision on whether the GPU path reproduces this batch. Reads only the batch.
bool accept_batch(const hwgs::Batch &batch, bool depth_available, DkGpuStats &stats) {
  const uint64_t prim = batch.state.effectivePrim;
  const auto &ctx = batch.state.ctx;
  const auto &env = batch.state.env;
  const bool tme = bit(prim, hwgs::primbit::kTme);
  const bool abe = bit(prim, hwgs::primbit::kAbe);

  if (bit(prim, hwgs::primbit::kAa1)) return reject(stats, Reject::Other);
  if ((env.colclamp & 1u) == 0) return reject(stats, Reject::ColClamp);

  const uint32_t frame_psm = hwgs::frame::Psm(ctx.frame);
  if (frame_psm != hwgs::psm::kCt32 && frame_psm != hwgs::psm::kCt24) return reject(stats, Reject::FramePsm);
  if (hwgs::frame::Fbmsk(ctx.frame) != 0) return reject(stats, Reject::FrameMask);

  const DepthUse depth = depth_use(ctx);
  if (depth.any() && !depth.never) {
    if (!depth_available) return reject(stats, Reject::DepthUnavailable);
    if (!z_format_known(z_psm_code(ctx.zbuf))) return reject(stats, Reject::ZFormat);
  }
  const AlphaPlan ap = alpha_plan(ctx, depth.write);
  if (ap.second_pass && abe) return reject(stats, Reject::AlphaFailBlend);
  if (hwgs::test::Date(ctx.test)) return reject(stats, Reject::DestAlphaTest);
  if (abe) {
    GpuBlend gb;
    if ((env.pabe & 1u) || !map_blend(ctx.alpha, gb)) return reject(stats, Reject::Blend);
  }

  bool lines = false, others = false;
  for (const auto &p : batch.prims) {
    if (p.type == hwgs::PrimType::Line || p.type == hwgs::PrimType::LineStrip) lines = true;
    else others = true;
  }
  if (lines && others) return reject(stats, Reject::PrimMix);

  if (tme) {
    const uint64_t tex0 = ctx.tex0;
    const uint32_t psm = hwgs::tex0::Psm(tex0);
    const bool known_psm = psm == hwgs::psm::kCt32 || psm == hwgs::psm::kCt24 || psm == hwgs::psm::kCt16 ||
                           psm == hwgs::psm::kCt16S || hwgs::IsIndexedPsm(psm);
    if (!known_psm || !memory::decodePsm(psm)) return reject(stats, Reject::Texture);
    if (hwgs::tex0::Tw(tex0) > 9u || hwgs::tex0::Th(tex0) > 9u) return reject(stats, Reject::Texture);
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
  return true;
}

struct VertexSetup {
  bool tme = false, fst = false, flat = false;
  float tex_w = 1.f, tex_h = 1.f;
  float y_sign = 1.f; // +1 puts GS row 0 at NDC y = -1 (upper-left origin); the self-test picks the sign
  double z_scale = 0.0;
  uint32_t z_max = 0;
};

// GS samples a pixel at its integer coordinate; the GPU at the pixel centre, hence the half-pixel shift.
GpuVert make_vert(const hwgs::Vertex &v, const hwgs::Vertex &colour_src, const VertexSetup &vs) {
  GpuVert o{};
  const float px = static_cast<float>(v.x4) / 16.f + 0.5f;
  const float py = static_cast<float>(v.y4) / 16.f + 0.5f;
  o.x = px / static_cast<float>(kDkSurface) * 2.f - 1.f;
  o.y = vs.y_sign * (py / static_cast<float>(kDkSurface) * 2.f - 1.f);
  o.z = static_cast<float>(static_cast<double>(std::min(v.z, vs.z_max)) * vs.z_scale);
  const hwgs::Vertex &c = vs.flat ? colour_src : v;
  o.r = c.r / 255.f;
  o.g = c.g / 255.f;
  o.b = c.b / 255.f;
  o.a = c.a / 255.f;
  o.fog = v.fog / 255.f;
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
  return o;
}

// Triangles, sprites and points become triangles; lines stay lines (the batch holds one kind).
void expand_prims(const hwgs::Batch &batch, const VertexSetup &vs, std::vector<GpuVert> &verts) {
  for (const auto &prim : batch.prims) {
    if (prim.first + prim.count > batch.vertices.size()) continue;
    const auto *pv = batch.vertices.data() + prim.first;
    switch (prim.type) {
    case hwgs::PrimType::Triangle:
    case hwgs::PrimType::TriangleStrip:
    case hwgs::PrimType::TriangleFan:
      // The front end records strips and fans as one primitive per kicked triangle; the last
      // vertex supplies the flat-shading colour.
      if (prim.count >= 3) {
        verts.push_back(make_vert(pv[0], pv[2], vs));
        verts.push_back(make_vert(pv[1], pv[2], vs));
        verts.push_back(make_vert(pv[2], pv[2], vs));
      }
      break;
    case hwgs::PrimType::Line:
    case hwgs::PrimType::LineStrip:
      if (prim.count >= 2) {
        verts.push_back(make_vert(pv[0], pv[1], vs));
        verts.push_back(make_vert(pv[1], pv[1], vs));
      }
      break;
    case hwgs::PrimType::Sprite:
    case hwgs::PrimType::Point: {
      // Sprites use the second vertex colour; a point is a one-pixel sprite.
      VertexSetup flat = vs;
      flat.flat = true;
      GpuVert a, b;
      if (prim.type == hwgs::PrimType::Point) {
        if (prim.count < 1) break;
        hwgs::Vertex far_corner = pv[0];
        far_corner.x4 += 16;
        far_corner.y4 += 16;
        a = make_vert(pv[0], pv[0], flat);
        b = make_vert(far_corner, pv[0], flat);
      } else {
        if (prim.count < 2) break;
        a = make_vert(pv[0], pv[1], flat);
        b = make_vert(pv[1], pv[1], flat);
        a.z = b.z; // sprites take Z from the second vertex
      }
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
      break;
    }
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
// Switch heap + nvmap are tight (especially in applet mode): keep this small. Larger textures
// reject to the CPU drawer. Release deko3d aborts the process on a failed dkMemBlockCreate.
constexpr int kTexSlots = 4;
constexpr uint32_t kTexSlotBytes = 512u * 512u * 4u; // max 512x512 RGBA8 per slot
constexpr int kWhiteSlot = kTexSlots;                // 1x1 white for untextured batches
constexpr int kImageDescriptors = kTexSlots + 1;
// Sampler index = linear * 4 + clamp S * 2 + clamp T (clamp = CLAMP or REGION_CLAMP).
constexpr int kSamplers = 8;
constexpr uint32_t kParamsBytes = DK_UNIFORM_BUF_ALIGNMENT;
constexpr uint32_t kSurfaceBytes = static_cast<uint32_t>(kDkSurface) * kDkSurface * 4u;

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

// GPU copy of one GS buffer (frame or Z), in surface pixel coordinates.
struct Surface {
  DkImage image{};
  DkMemBlock mem = nullptr;
  bool is_depth = false;
  bool valid = false;           // view below is meaningful
  memory::BufferView view{0, 0, memory::Psm::Ct32};
  uint32_t fbw = 0;
  uint32_t psm_code = 0;
  Rect seeded;                  // area whose GPU copy equals SLM (or holds newer pixels inside `dirty`)
  Rect dirty;                   // area drawn on the GPU and not yet written back

  bool same_target(const memory::BufferView &v) const {
    return valid && view.basePointer == v.basePointer && view.bufferWidth == v.bufferWidth && view.psm == v.psm;
  }
  // Conservative block range of the rows the GPU copy holds.
  bool overlaps(uint32_t block, uint32_t blocks) const {
    const Rect held = unite(seeded, dirty);
    if (!valid || held.empty()) return false;
    return hwgs::BlockRangesOverlap(block, blocks, view.basePointer,
                                    hwgs::FootprintBlocks(fbw, static_cast<uint32_t>(held.y1 + 1)));
  }
  bool dirty_overlaps(uint32_t block, uint32_t blocks) const {
    if (!valid || dirty.empty()) return false;
    return hwgs::BlockRangesOverlap(block, blocks, view.basePointer,
                                    hwgs::FootprintBlocks(fbw, static_cast<uint32_t>(dirty.y1 + 1)));
  }
};

constexpr unsigned kColourBit = 1u, kDepthBit = 2u;

} // namespace

struct WritebackDk::State {
  DkDevice device = nullptr;
  DkQueue queue = nullptr;
  DkCmdBuf cmdbuf = nullptr;
  DkMemBlock cmd_mem = nullptr;
  DkMemBlock code_mem = nullptr;
  DkMemBlock data_mem = nullptr;
  DkMemBlock stage_mem = nullptr;
  DkMemBlock desc_mem = nullptr;
  DkMemBlock tex_mem = nullptr;
  DkMemBlock ubo_mem = nullptr;
  DkShader vshader{};
  DkShader fshader{};
  Surface colour;
  Surface depth;
  bool depth_available = false;
  float y_sign = 1.f;
  std::uint8_t *stage_cpu = nullptr;
  std::uint8_t *data_cpu = nullptr;
  std::uint8_t *tex_cpu = nullptr;
  DkImageDescriptor *image_desc = nullptr;
  DkSamplerDescriptor *sampler_desc = nullptr;
  uint32_t sampler_desc_offset = 0;
  uint32_t vert_capacity = 0;
  uint32_t vert_offset = 0; // in vertices
  uint32_t cmd_bytes = 0;
  bool recording = false;
  bool descriptors_bound = false;
  int next_tex_slot = 0;
  TexSlot tex_slots[kTexSlots]{};
  TexSlot white{};
  std::unordered_map<TexKey, std::vector<std::uint32_t>, TexKeyHash> tex_cache;
  std::uint64_t tex_cache_bytes = 0;
  // CPU backing for memblocks created with maker.storage (we free these; deko3d does not).
  void *owned_cmd = nullptr;
  void *owned_code = nullptr;
  void *owned_colour = nullptr;
  void *owned_depth = nullptr;
  void *owned_tex = nullptr;
  void *owned_data = nullptr;
  void *owned_stage = nullptr;
  void *owned_ubo = nullptr;
  void *owned_desc = nullptr;

  bool in_flight = false;  // a submitted list may still be running
  bool gpu_error = false;  // the queue faulted: no further GPU work
  void begin_recording() {
    if (!recording) {
      // dkCmdBufClear rewinds the command memory, which a running list may still be reading.
      if (in_flight) {
        dkQueueWaitIdle(queue);
        in_flight = false;
      }
      dkCmdBufClear(cmdbuf);
      recording = true;
      descriptors_bound = false;
    }
  }
  void submit(DkGpuStats &stats) {
    if (recording) {
      const DkCmdList list = dkCmdBufFinishList(cmdbuf);
      recording = false;
      // Submitting to a faulted queue is a fatal deko3d error.
      if (gpu_error || dkQueueIsInErrorState(queue)) {
        gpu_error = true;
        return;
      }
      dkQueueSubmitCommands(queue, list);
      in_flight = true;
      ++stats.submissions;
    }
  }
  // Orders copy-engine and render work recorded in the same list.
  void barrier() {
    dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache | DkInvalidateFlags_Zcull);
  }
  void submit_and_wait(DkGpuStats &stats) {
    submit(stats);
    dkQueueWaitIdle(queue);
    in_flight = false;
    if (!gpu_error && dkQueueIsInErrorState(queue)) {
      gpu_error = true;
      std::fprintf(stderr, "[hw-dk] GPU queue error after %llu submissions: GPU drawer off, CPU draws the rest\n",
                   static_cast<unsigned long long>(stats.submissions));
    }
    vert_offset = 0;
    for (auto &slot : tex_slots) slot.used_since_wait = false;
  }
};

namespace {

using State = WritebackDk::State;

// Writes the dirty parts of the selected surfaces back into SLM. One surface at a time so the
// staging buffer can stay a single 1024x1024 RGBA/float page (~4 MiB) instead of two.
bool flush_surfaces(State &s, DkGpuStats &stats, memory::LocalMemory &local, unsigned mask, std::string &error) {
  Surface *surfaces[2] = {(mask & kColourBit) ? &s.colour : nullptr, (mask & kDepthBit) ? &s.depth : nullptr};
  bool any = false;
  for (int i = 0; i < 2; ++i) {
    if (!surfaces[i] || !surfaces[i]->valid || surfaces[i]->dirty.empty()) continue;
    Rect r = intersect(surfaces[i]->dirty, Rect{0, 0, kDkSurface - 1, kDkSurface - 1});
    surfaces[i]->dirty = {};
    if (r.empty()) continue;
    any = true;
    Surface &surf = *surfaces[i];
    const uint32_t rw = static_cast<uint32_t>(r.x1 - r.x0 + 1), rh = static_cast<uint32_t>(r.y1 - r.y0 + 1);
    s.begin_recording();
    s.barrier();
    DkImageView view;
    dkImageViewDefaults(&view, &surf.image);
    DkImageRect rect{static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), 0, rw, rh, 1};
    DkCopyBuf dst{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
    dkCmdBufCopyImageToBuffer(s.cmdbuf, &view, &rect, &dst, 0);
    s.submit_and_wait(stats);
    auto *words = reinterpret_cast<std::uint32_t *>(s.stage_cpu);
    if (surf.is_depth) {
      const double inv = 1.0 / z_scale(surf.psm_code);
      const uint32_t zmax = z_max(surf.psm_code);
      for (std::size_t n = 0; n < static_cast<std::size_t>(rw) * rh; ++n) {
        float d;
        std::memcpy(&d, &words[n], 4);
        const double z = std::floor(static_cast<double>(d) * inv + 0.5);
        words[n] = z <= 0.0 ? 0u : (z >= zmax ? zmax : static_cast<uint32_t>(z));
      }
    }
    local.writeRect(surf.view, static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), rw, rh, words);
    stats.readback_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
    ++stats.flushes;
  }
  if (!any) s.submit(stats);
  (void)error;
  return true;
}

// Copies SLM into the GPU surface so it covers `area`. The surface must have no dirty pixels.
void seed_surface(State &s, DkGpuStats &stats, memory::LocalMemory &local, Surface &surf, const Rect &area) {
  const Rect seed = unite(surf.seeded, area);
  const uint32_t rw = static_cast<uint32_t>(seed.x1 - seed.x0 + 1);
  const uint32_t rh = static_cast<uint32_t>(seed.y1 - seed.y0 + 1);
  // Staging is shared across surfaces; wait before overwriting it.
  s.submit_and_wait(stats);
  auto *words = reinterpret_cast<std::uint32_t *>(s.stage_cpu);
  local.readRect(surf.view, static_cast<uint32_t>(seed.x0), static_cast<uint32_t>(seed.y0), rw, rh, words);
  if (surf.is_depth) {
    const double scale = z_scale(surf.psm_code);
    const uint32_t zmax = z_max(surf.psm_code);
    for (std::size_t n = 0; n < static_cast<std::size_t>(rw) * rh; ++n) {
      const float d = static_cast<float>(static_cast<double>(std::min(words[n], zmax)) * scale);
      std::memcpy(&words[n], &d, 4);
    }
  }
  s.begin_recording();
  DkImageView view;
  dkImageViewDefaults(&view, &surf.image);
  DkCopyBuf src{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
  DkImageRect rect{static_cast<uint32_t>(seed.x0), static_cast<uint32_t>(seed.y0), 0, rw, rh, 1};
  dkCmdBufCopyBufferToImage(s.cmdbuf, &src, &view, &rect, 0);
  s.barrier();
  stats.upload_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
  ++stats.seeds;
  surf.seeded = seed;
}

// Pre-allocate CPU storage so a heap OOM returns false instead of release-deko3d's
// diagAbortWithResult (cbDebug is debug-library only; release always aborts on DK_ERROR).
DkMemBlock create_memblock(DkDevice device, uint32_t size, uint32_t flags, void **owned_storage,
                           std::string &error, const char *name) {
  size = (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
  std::fprintf(stderr, "[hw-dk] alloc %s (%u KiB)\n", name, size / 1024u);
  std::fflush(stderr);
  // memalign: aligned_alloc is spotty on newlib; deko3d requires DK_MEMBLOCK_ALIGNMENT.
  void *storage = memalign(DK_MEMBLOCK_ALIGNMENT, size);
  if (!storage) {
    error = std::string(name) + " heap OOM";
    std::fprintf(stderr, "[hw-dk] %s\n", error.c_str());
    return nullptr;
  }
  std::memset(storage, 0, size);
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, device, size);
  mm.flags = flags;
  mm.storage = storage;
  DkMemBlock block = dkMemBlockCreate(&mm);
  if (!block) {
    // Reachable only with the debug library; release aborts inside create.
    std::free(storage);
    error = std::string(name) + " dkMemBlockCreate failed";
    return nullptr;
  }
  *owned_storage = storage;
  return block;
}

bool make_surface_image(DkDevice device, Surface &surf, DkImageFormat format, uint32_t flags, void **owned,
                        std::string &error, const char *name) {
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, device);
  lm.flags = flags;
  lm.format = format;
  lm.dimensions[0] = kDkSurface;
  lm.dimensions[1] = kDkSurface;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &lm);
  uint32_t size = static_cast<uint32_t>(dkImageLayoutGetSize(&layout));
  const uint32_t align = dkImageLayoutGetAlignment(&layout);
  size = (size + align - 1) & ~(align - 1);
  surf.mem = create_memblock(device, size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, owned, error, name);
  if (!surf.mem) return false;
  dkImageInitialize(&surf.image, &layout, surf.mem, 0);
  return true;
}

} // namespace
#endif

namespace {
bool g_allowed = true;
}

void WritebackDk::set_allowed(bool allowed) { g_allowed = allowed; }

bool WritebackDk::init(const DkHandles &handles, std::string &error) {
  shutdown();
#if !defined(__SWITCH__)
  (void)handles;
  error = "WritebackDk requires __SWITCH__";
  return false;
#else
  if (!g_allowed) {
    error = "GPU drawer disabled by gpudraw.off";
    return false;
  }
  std::fprintf(stderr, "[hw-dk] WritebackDk init\n");
  if (!handles.device || !handles.queue) {
    error = "WritebackDk: missing device/queue";
    return false;
  }
  handles_ = handles;
  state_ = new State();
  State &s = *state_;
  s.device = static_cast<DkDevice>(handles.device);
  s.queue = static_cast<DkQueue>(handles.queue);

  // Budget (MiB): colour~4 + depth~4 + tex~4 + verts~2 + stage~4 + misc~1 ≈ 19, down from ~40.
  // Present's 1280x720 swapchain already sits beside this. Release deko3d aborts on a failed
  // dkMemBlockCreate (cbDebug is debug-only), so we pre-allocate heap storage first.
  s.cmd_bytes = 256 * 1024;
  s.cmd_mem = create_memblock(s.device, s.cmd_bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                              &s.owned_cmd, error, "cmd");
  if (!s.cmd_mem) {
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
  s.code_mem =
      create_memblock(s.device, code_bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                      &s.owned_code, error, "code");
  if (!s.code_mem) {
    shutdown();
    return false;
  }
  uint32_t code_off = 0;
  if (!load_shader(s.code_mem, code_off, gs_draw_vsh_dksh_bytes, gs_draw_vsh_dksh_size, s.vshader, error) ||
      !load_shader(s.code_mem, code_off, gs_draw_fsh_dksh_bytes, gs_draw_fsh_dksh_size, s.fshader, error)) {
    shutdown();
    return false;
  }

  std::fprintf(stderr, "[hw-dk] init: shaders loaded\n");
  if (!make_surface_image(s.device, s.colour, DkImageFormat_RGBA8_Unorm,
                          DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, &s.owned_colour, error,
                          "colour") ||
      !make_surface_image(s.device, s.depth, DkImageFormat_ZF32, DkImageFlags_UsageRender, &s.owned_depth, error,
                          "depth")) {
    shutdown();
    return false;
  }
  std::fprintf(stderr, "[hw-dk] init: colour and depth surfaces created\n");
  s.depth.is_depth = true;

  const uint32_t tex_bytes = kTexSlots * kTexSlotBytes + 0x1000u; // last page: the white texel
  s.tex_mem = create_memblock(s.device, tex_bytes,
                              DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                              &s.owned_tex, error, "tex");
  if (!s.tex_mem) {
    shutdown();
    return false;
  }
  s.tex_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.tex_mem));
  for (int i = 0; i < kTexSlots; ++i) s.tex_slots[i].mem_off = static_cast<uint32_t>(i) * kTexSlotBytes;
  s.white.mem_off = kTexSlots * kTexSlotBytes;
  reinterpret_cast<std::uint32_t *>(s.tex_cpu + s.white.mem_off)[0] = 0xFFFFFFFFu;
  {
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, s.device);
    lm.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = 1;
    lm.dimensions[1] = 1;
    lm.pitchStride = DK_IMAGE_LINEAR_STRIDE_ALIGNMENT;
    dkImageLayoutInitialize(&s.white.layout, &lm);
    dkImageInitialize(&s.white.image, &s.white.layout, s.tex_mem, s.white.mem_off);
  }

  const uint32_t vert_bytes = 2u * 1024u * 1024u;
  s.data_mem = create_memblock(s.device, vert_bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                               &s.owned_data, error, "verts");
  if (!s.data_mem) {
    shutdown();
    return false;
  }
  s.data_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.data_mem));
  s.vert_capacity = vert_bytes / sizeof(GpuVert);

  s.stage_mem = create_memblock(s.device, kSurfaceBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                &s.owned_stage, error, "stage");
  if (!s.stage_mem) {
    shutdown();
    return false;
  }
  s.stage_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.stage_mem));

  s.ubo_mem = create_memblock(s.device, kParamsBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                              &s.owned_ubo, error, "ubo");
  if (!s.ubo_mem) {
    shutdown();
    return false;
  }

  s.desc_mem = create_memblock(s.device, 0x1000, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                               &s.owned_desc, error, "desc");
  if (!s.desc_mem) {
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
  std::fprintf(stderr, "[hw-dk] init: buffers and descriptors ready, running self-test\n");
  if (!self_test(error)) {
    std::fprintf(stderr, "[hw-dk] WritebackDk self-test failed: %s (GPU drawer off, CPU draws everything)\n",
                 error.c_str());
    shutdown();
    return false;
  }
  std::fprintf(stderr, "[hw-dk] WritebackDk GPU drawer ready (%dx%d, y_sign=%+.0f, depth=%s)\n", kDkSurface,
               kDkSurface, s.y_sign, s.depth_available ? "on" : "off");
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
  for (DkMemBlock m : {s.cmd_mem, s.desc_mem, s.ubo_mem, s.stage_mem, s.data_mem, s.tex_mem, s.colour.mem,
                       s.depth.mem, s.code_mem}) {
    if (m) dkMemBlockDestroy(m);
  }
  for (void *p : {s.owned_cmd, s.owned_code, s.owned_colour, s.owned_depth, s.owned_tex, s.owned_data, s.owned_stage,
                  s.owned_ubo, s.owned_desc}) {
    std::free(p);
  }
  delete state_;
  state_ = nullptr;
#endif
  handles_ = {};
  stats_ = {};
}

bool WritebackDk::self_test(std::string &error) {
#if !defined(__SWITCH__)
  error = "not switch";
  return false;
#else
  // Draws known sprites into a scratch local memory and reads them back: picks the NDC y
  // direction for GS row 0 and checks that depth seeding, testing and write-back round-trip.
  State &s = *state_;
  auto local = std::make_unique<memory::LocalMemory>();
  CounterSet counters;
  clut::ClutUnit clut(counters);
  constexpr uint32_t kFbp = 0, kFbw = 10, kZbp = 200;
  const memory::BufferView frame_view{memory::blockOfPage(kFbp), kFbw, memory::Psm::Ct32};
  const memory::BufferView z_view{memory::blockOfPage(kZbp), kFbw, memory::Psm::Z24};

  auto make_sprite = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t z, uint8_t r, uint8_t g,
                         bool depth) {
    hwgs::Batch b;
    b.state.ctx.frame = (uint64_t{kFbw} << 16) | kFbp; // PSM CT32, FBMSK 0
    b.state.ctx.zbuf = depth ? ((uint64_t{1} << 24) | kZbp) : (uint64_t{1} << 32); // Z24 written / masked
    b.state.ctx.test = depth ? ((uint64_t{1} << 16) | (uint64_t{kZtstGequal} << 17)) : 0;
    b.state.ctx.scissor = (uint64_t{1023} << 16) | (uint64_t{1023} << 48);
    b.state.env.colclamp = 1;
    b.state.effectivePrim = static_cast<uint64_t>(hwgs::PrimType::Sprite);
    hwgs::Vertex a{}, c{};
    a.x4 = x0 * 16;
    a.y4 = y0 * 16;
    c.x4 = x1 * 16;
    c.y4 = y1 * 16;
    a.z = c.z = z;
    a.r = c.r = r;
    a.g = c.g = g;
    a.a = c.a = 0x80;
    b.vertices = {a, c};
    hwgs::PrimRecord p;
    p.type = hwgs::PrimType::Sprite;
    p.first = 0;
    p.count = 2;
    p.rect = {x0, y0, x1 - 1, y1 - 1};
    b.prims = {p};
    b.drawRect = p.rect;
    return b;
  };
  auto reset_surfaces = [&]() {
    s.submit_and_wait(stats_);
    s.colour.valid = s.depth.valid = false;
    s.colour.seeded = s.colour.dirty = s.depth.seeded = s.depth.dirty = {};
    invalidate_textures();
  };
  auto red_at = [&](uint32_t x, uint32_t y) { return (local->readPixel(frame_view, x, y) & 0xFFFFFFu) == 0x0000FFu; };

  bool oriented = false;
  for (float sign : {1.f, -1.f}) {
    s.y_sign = sign;
    reset_surfaces();
    std::vector<std::uint32_t> zero(16 * 16, 0);
    local->writeRect(frame_view, 0, 0, 16, 16, zero.data());
    std::fprintf(stderr, "[hw-dk] self-test: colour sprite, y_sign=%+.0f\n", sign);
    if (!draw_batch(make_sprite(0, 0, 8, 8, 0, 255, 0, false), *local, clut, error)) return false;
    flush_to_slm(*local, error);
    std::fprintf(stderr, "[hw-dk] self-test: colour read back, pixel(2,2)=0x%08X queue_error=%d\n",
                 local->readPixel(frame_view, 2, 2), s.gpu_error ? 1 : 0);
    if (s.gpu_error) {
      error = "GPU queue error during the colour self-test";
      return false;
    }
    if (red_at(2, 2) && red_at(7, 7) && !red_at(8, 8)) {
      oriented = true;
      break;
    }
  }
  if (!oriented) {
    error = "colour sprite did not land at GS (0,0)-(7,7) with either y direction";
    reset_surfaces();
    stats_ = {};
    return false;
  }

  s.depth_available = true;
  reset_surfaces();
  std::vector<std::uint32_t> zero(16 * 16, 0), z1000(16 * 16, 1000);
  local->writeRect(frame_view, 0, 0, 16, 16, zero.data());
  local->writeRect(z_view, 0, 0, 16, 16, z1000.data());
  std::fprintf(stderr, "[hw-dk] self-test: depth sprites\n");
  bool depth_ok = draw_batch(make_sprite(0, 0, 8, 8, 2000, 255, 0, true), *local, clut, error) &&
                  draw_batch(make_sprite(0, 0, 4, 4, 500, 0, 255, true), *local, clut, error);
  flush_to_slm(*local, error);
  std::fprintf(stderr, "[hw-dk] self-test: depth read back, queue_error=%d\n", s.gpu_error ? 1 : 0);
  depth_ok = depth_ok && !s.gpu_error && red_at(2, 2) && red_at(6, 6) && (local->readPixel(z_view, 2, 2) & 0xFFFFFFu) == 2000u &&
             (local->readPixel(z_view, 12, 12) & 0xFFFFFFu) == 1000u;
  if (!depth_ok) {
    std::fprintf(stderr, "[hw-dk] depth self-test failed (pixel 0x%08X z %u): depth batches stay on the CPU\n",
                 local->readPixel(frame_view, 2, 2), local->readPixel(z_view, 2, 2) & 0xFFFFFFu);
    s.depth_available = false;
  }
  error.clear();
  reset_surfaces();
  stats_ = {};
  return true;
#endif
}

void WritebackDk::report(std::FILE *out) const {
  if (!out) return;
  std::fprintf(out,
               "[hw-dk] GPU batches=%llu (depth %llu, mip base level %llu) cpu_fallback=%llu seeds=%llu flushes=%llu "
               "gpu_scanouts=%llu readback_MiB=%.1f upload_MiB=%.1f\n",
               static_cast<unsigned long long>(stats_.batches), static_cast<unsigned long long>(stats_.depth_batches),
               static_cast<unsigned long long>(stats_.mip_base_level),
               static_cast<unsigned long long>(stats_.cpu_fallback), static_cast<unsigned long long>(stats_.seeds),
               static_cast<unsigned long long>(stats_.flushes), static_cast<unsigned long long>(stats_.gpu_scanouts),
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
  state_->colour.seeded = {};
  state_->depth.seeded = {};
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
  for (auto &slot : state_->tex_slots) {
    slot.occupied = false;
    slot.key = {};
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
  unsigned mask = 0;
  if (s.colour.dirty_overlaps(block, blocks)) mask |= kColourBit;
  if (s.depth.dirty_overlaps(block, blocks)) mask |= kDepthBit;
  if (mask) {
    std::string err;
    flush_surfaces(s, stats_, local, mask, err);
    invalidate_textures();
  }
  if (write) {
    if (s.colour.overlaps(block, blocks)) s.colour.seeded = {};
    if (s.depth.overlaps(block, blocks)) s.depth.seeded = {};
    invalidate_textures();
  }
#endif
}

bool WritebackDk::flush_to_slm(memory::LocalMemory &local, std::string &error, bool colour_only) {
#if !defined(__SWITCH__)
  (void)local;
  (void)colour_only;
  error = "not switch";
  return false;
#else
  if (!ready_ || !state_) return true;
  State &s = *state_;
  const bool had_dirty = !s.colour.dirty.empty() || (!colour_only && !s.depth.dirty.empty());
  if (!flush_surfaces(s, stats_, local, colour_only ? kColourBit : (kColourBit | kDepthBit), error)) return false;
  // Pixels just written back may be sampled as a texture later.
  if (had_dirty) invalidate_textures();
  return true;
#endif
}

bool WritebackDk::try_scanout(std::uint32_t fbp_pages, std::uint32_t fbw, std::uint32_t psm, std::int32_t dbx,
                              std::int32_t dby, std::uint32_t read_w, std::uint32_t read_h, std::uint32_t display_w,
                              std::uint32_t display_h, std::vector<std::uint32_t> &rgba, std::string &error) {
#if !defined(__SWITCH__)
  (void)fbp_pages;
  (void)fbw;
  (void)psm;
  (void)dbx;
  (void)dby;
  (void)read_w;
  (void)read_h;
  (void)display_w;
  (void)display_h;
  (void)rgba;
  error = "not switch";
  return false;
#else
  if (!ready_ || !state_ || state_->gpu_error) return false;
  if (read_w == 0 || read_h == 0 || display_w == 0 || display_h == 0) return false;
  if (psm != hwgs::psm::kCt32 && psm != hwgs::psm::kCt24) return false;
  // Present either matches the read rectangle or is a simple FFMD line-double of it.
  const bool line_double = display_w == read_w && display_h == read_h * 2u;
  if (!(display_w == read_w && display_h == read_h) && !line_double) return false;

  State &s = *state_;
  const auto decoded = memory::decodePsm(psm);
  if (!decoded) return false;
  const memory::BufferView view{memory::blockOfPage(fbp_pages), fbw, *decoded};
  if (!s.colour.same_target(view)) return false;

  const Rect need{dbx, dby, dbx + static_cast<int32_t>(read_w) - 1, dby + static_cast<int32_t>(read_h) - 1};
  if (need.x0 < 0 || need.y0 < 0 || need.x1 >= kDkSurface || need.y1 >= kDkSurface) return false;
  const Rect held = unite(s.colour.seeded, s.colour.dirty);
  if (!held.contains(need)) return false;

  const uint32_t rw = read_w, rh = read_h;
  if (static_cast<std::uint64_t>(rw) * rh * 4u > kSurfaceBytes) return false;

  s.begin_recording();
  s.barrier();
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &s.colour.image);
  DkImageRect rect{static_cast<uint32_t>(need.x0), static_cast<uint32_t>(need.y0), 0, rw, rh, 1};
  DkCopyBuf dst{dkMemBlockGetGpuAddr(s.stage_mem), 0, 0};
  dkCmdBufCopyImageToBuffer(s.cmdbuf, &colour_view, &rect, &dst, 0);
  s.submit_and_wait(stats_);
  if (s.gpu_error) return false;

  const auto *src = reinterpret_cast<const std::uint32_t *>(s.stage_cpu);
  rgba.resize(static_cast<std::size_t>(display_w) * display_h);
  if (line_double) {
    for (uint32_t y = 0; y < rh; ++y) {
      const std::uint32_t *row = src + static_cast<std::size_t>(y) * rw;
      std::memcpy(rgba.data() + static_cast<std::size_t>(y * 2u) * display_w, row, rw * 4u);
      std::memcpy(rgba.data() + static_cast<std::size_t>(y * 2u + 1u) * display_w, row, rw * 4u);
    }
  } else {
    std::memcpy(rgba.data(), src, static_cast<std::size_t>(rw) * rh * 4u);
  }
  stats_.readback_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
  ++stats_.gpu_scanouts;
  (void)error;
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
  if (s.gpu_error) return reject(stats_, Reject::Other);
  if (!accept_batch(batch, s.depth_available, stats_)) return false;

  const auto &ctx = batch.state.ctx;
  const uint64_t prim = batch.state.effectivePrim;
  const bool tme = bit(prim, hwgs::primbit::kTme);
  const bool abe = bit(prim, hwgs::primbit::kAbe);
  const DepthUse depth = depth_use(ctx);
  if (depth.never) return true; // every pixel fails the depth test: nothing is written

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

  // Targets. A different frame or Z buffer: write the old one back and start a fresh GPU copy.
  const uint32_t frame_psm_code = hwgs::frame::Psm(ctx.frame);
  const uint32_t fbw = hwgs::frame::Fbw(ctx.frame);
  const memory::BufferView frame_view{memory::blockOfPage(hwgs::frame::Fbp(ctx.frame)), fbw,
                                      *memory::decodePsm(frame_psm_code)};
  if (!s.colour.same_target(frame_view)) {
    if (!flush_surfaces(s, stats_, local, kColourBit, error)) return false;
    s.colour.seeded = {};
  }
  s.colour.view = frame_view;
  s.colour.fbw = fbw;
  s.colour.psm_code = frame_psm_code;
  s.colour.valid = true;

  const bool uses_depth = depth.any();
  if (uses_depth) {
    const uint32_t zcode = z_psm_code(ctx.zbuf);
    const memory::BufferView z_view{memory::blockOfPage(hwgs::zbuf::Zbp(ctx.zbuf)), fbw, *memory::decodePsm(zcode)};
    if (!s.depth.same_target(z_view)) {
      if (!flush_surfaces(s, stats_, local, kDepthBit, error)) return false;
      s.depth.seeded = {};
    }
    s.depth.view = z_view;
    s.depth.fbw = fbw;
    s.depth.psm_code = zcode;
    s.depth.valid = true;
  }

  // A texture read from an area the GPU holds newer pixels for needs them in SLM first.
  if (tme) {
    const uint64_t tex0 = ctx.tex0;
    const uint32_t tex_blocks = hwgs::FootprintBlocks(hwgs::tex0::Tbw(tex0), 1u << hwgs::tex0::Th(tex0));
    unsigned mask = 0;
    if (s.colour.dirty_overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks)) mask |= kColourBit;
    if (s.depth.dirty_overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks)) mask |= kDepthBit;
    if (mask && !flush_to_slm(local, error, mask == kColourBit)) return false;
  }

  // Grow the GPU copies to cover this batch. SLM must hold the surface's GPU pixels first.
  if (!s.colour.seeded.contains(area)) {
    if (!s.colour.dirty.empty() && !flush_surfaces(s, stats_, local, kColourBit, error)) return false;
    seed_surface(s, stats_, local, s.colour, area);
  }
  if (uses_depth && !s.depth.seeded.contains(area)) {
    if (!s.depth.dirty.empty() && !flush_surfaces(s, stats_, local, kDepthBit, error)) return false;
    seed_surface(s, stats_, local, s.depth, area);
  }

  // Texture: decoded once per (TEX0, TEXA, CLUT) until local memory changes, uploaded into a slot.
  Params params{};
  int image_index = kWhiteSlot;
  VertexSetup vs;
  vs.tme = tme;
  vs.fst = bit(prim, hwgs::primbit::kFst);
  vs.flat = !bit(prim, hwgs::primbit::kIip);
  vs.y_sign = s.y_sign;
  if (uses_depth) {
    vs.z_scale = z_scale(s.depth.psm_code);
    vs.z_max = z_max(s.depth.psm_code);
  }
  int sampler_index = 0;
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
      constexpr std::uint64_t kCacheCap = 12ull * 1024ull * 1024ull;
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
        s.submit_and_wait(stats_);
        slot_i = s.next_tex_slot % kTexSlots;
      }
      s.next_tex_slot = (slot_i + 1) % kTexSlots;
      TexSlot &slot = s.tex_slots[slot_i];
      const uint32_t pitch =
          (tw * 4u + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
      if (static_cast<std::uint64_t>(pitch) * th > kTexSlotBytes) return reject(stats_, Reject::Texture);
      std::uint8_t *dst = s.tex_cpu + slot.mem_off;
      for (uint32_t y = 0; y < th; ++y)
        std::memcpy(dst + y * pitch, texels->data() + static_cast<std::size_t>(y) * tw, tw * 4u);
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
    const uint32_t wms = hwgs::clampreg::Wms(ctx.clamp), wmt = hwgs::clampreg::Wmt(ctx.clamp);
    const bool linear = hwgs::tex1::Mmag(ctx.tex1) != 0u;
    sampler_index = (linear ? 4 : 0) + (wms == 1u || wms == 2u ? 2 : 0) + (wmt == 1u || wmt == 2u ? 1 : 0);
    if (hwgs::tex1::Mxl(ctx.tex1) > 0u && hwgs::tex1::Mmin(ctx.tex1) >= 2u) ++stats_.mip_base_level;
    params.texfn[0] = 1.f;
    params.texfn[1] = static_cast<float>(hwgs::tex0::Tfx(ctx.tex0));
    params.texfn[2] = static_cast<float>(hwgs::tex0::Tcc(ctx.tex0));
    params.wrap[0] = static_cast<float>(wms);
    params.wrap[1] = static_cast<float>(wmt);
    params.wrap[2] = vs.tex_w;
    params.wrap[3] = vs.tex_h;
    params.region[0] = static_cast<float>(hwgs::clampreg::Minu(ctx.clamp));
    params.region[1] = static_cast<float>(hwgs::clampreg::Maxu(ctx.clamp));
    params.region[2] = static_cast<float>(hwgs::clampreg::Minv(ctx.clamp));
    params.region[3] = static_cast<float>(hwgs::clampreg::Maxv(ctx.clamp));
  }
  if (bit(prim, hwgs::primbit::kFge)) {
    const uint64_t fc = batch.state.env.fogcol;
    params.texfn[3] = 1.f;
    params.fogcol[0] = static_cast<float>(fc & 0xFFu);
    params.fogcol[1] = static_cast<float>((fc >> 8) & 0xFFu);
    params.fogcol[2] = static_cast<float>((fc >> 16) & 0xFFu);
  }
  const AlphaPlan ap = alpha_plan(ctx, depth.write);
  params.test[0] = static_cast<float>(ap.atst);
  params.test[1] = ap.aref;
  params.test[3] = (ctx.fba & 1u) ? 1.f : 0.f;

  std::vector<GpuVert> verts;
  verts.reserve(batch.vertices.size() * 3);
  expand_prims(batch, vs, verts);
  if (verts.empty()) return true;
  if (verts.size() > s.vert_capacity) return reject(stats_, Reject::Other);
  if (s.vert_offset + verts.size() > s.vert_capacity) s.submit_and_wait(stats_);
  const bool lines = batch.prims.front().type == hwgs::PrimType::Line ||
                     batch.prims.front().type == hwgs::PrimType::LineStrip;

  const uint32_t base = s.vert_offset;
  std::memcpy(s.data_cpu + base * sizeof(GpuVert), verts.data(), verts.size() * sizeof(GpuVert));
  s.vert_offset = base + static_cast<uint32_t>(verts.size());

  s.begin_recording();
  DkImageView colour_view, depth_view;
  dkImageViewDefaults(&colour_view, &s.colour.image);
  dkImageViewDefaults(&depth_view, &s.depth.image);
  dkCmdBufBindRenderTarget(s.cmdbuf, &colour_view, uses_depth ? &depth_view : nullptr);
  DkViewport vp{0, 0, (float)kDkSurface, (float)kDkSurface, 0, 1};
  DkScissor sc{static_cast<uint32_t>(area.x0), static_cast<uint32_t>(area.y0), static_cast<uint32_t>(area.x1 - area.x0 + 1),
               static_cast<uint32_t>(area.y1 - area.y0 + 1)};
  dkCmdBufSetViewports(s.cmdbuf, 0, &vp, 1);
  dkCmdBufSetScissors(s.cmdbuf, 0, &sc, 1);
  if (lines) dkCmdBufSetLineWidth(s.cmdbuf, 1.f);

  const DkShader *shaders[] = {&s.vshader, &s.fshader};
  dkCmdBufBindShaders(s.cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(s.cmdbuf, &rs);

  GpuBlend gb;
  if (abe) map_blend(ctx.alpha, gb);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, gb.enable);
  dkCmdBufBindColorState(s.cmdbuf, &cs);
  if (gb.enable) {
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkBlendStateSetFactors(&bs, gb.src, gb.dst, DkBlendFactor_One, DkBlendFactor_Zero);
    dkBlendStateSetOps(&bs, gb.op, DkBlendOp_Add);
    dkCmdBufBindBlendState(s.cmdbuf, 0, &bs);
    dkCmdBufSetBlendConst(s.cmdbuf, 0.f, 0.f, 0.f, gb.constant);
  }

  if (!s.descriptors_bound) {
    dkCmdBufBindImageDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem), kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(s.cmdbuf, dkMemBlockGetGpuAddr(s.desc_mem) + s.sampler_desc_offset, kSamplers);
    dkCmdBufBindUniformBuffer(s.cmdbuf, DkStage_Fragment, 0, dkMemBlockGetGpuAddr(s.ubo_mem), kParamsBytes);
    s.descriptors_bound = true;
  }
  dkCmdBufBindTexture(s.cmdbuf, DkStage_Fragment, 0, dkMakeTextureHandle(image_index, sampler_index));

  DkVtxAttribState attrs[4] = {
      {0, 0, offsetof(GpuVert, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, s), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, offsetof(GpuVert, fog), DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
  };
  DkVtxBufferState bufstate{sizeof(GpuVert), 0};
  dkCmdBufBindVtxAttribState(s.cmdbuf, attrs, 4);
  dkCmdBufBindVtxBufferState(s.cmdbuf, &bufstate, 1);
  dkCmdBufBindVtxBuffer(s.cmdbuf, 0, dkMemBlockGetGpuAddr(s.data_mem) + base * sizeof(GpuVert),
                        static_cast<uint32_t>(verts.size() * sizeof(GpuVert)));

  const DkCompareOp z_op = !depth.test ? DkCompareOp_Always
                                       : (hwgs::test::Ztst(ctx.test) == kZtstGequal ? DkCompareOp_Gequal
                                                                                     : DkCompareOp_Greater);
  auto draw_pass = [&](bool invert, bool write_colour, bool write_alpha, bool write_depth) {
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = uses_depth;
    ds.depthWriteEnable = uses_depth && write_depth;
    ds.depthCompareOp = z_op;
    ds.stencilTestEnable = false;
    dkCmdBufBindDepthStencilState(s.cmdbuf, &ds);
    DkColorWriteState cws;
    dkColorWriteStateDefaults(&cws);
    dkColorWriteStateSetMask(&cws, 0, write_colour ? (write_alpha ? DkColorMask_RGBA : DkColorMask_RGB) : 0u);
    dkCmdBufBindColorWriteState(s.cmdbuf, &cws);
    params.test[2] = invert ? 1.f : 0.f;
    dkCmdBufPushConstants(s.cmdbuf, dkMemBlockGetGpuAddr(s.ubo_mem), kParamsBytes, 0, sizeof(Params), &params);
    dkCmdBufDraw(s.cmdbuf, lines ? DkPrimitive_Lines : DkPrimitive_Triangles, static_cast<uint32_t>(verts.size()), 1, 0, 0);
  };
  draw_pass(false, true, true, depth.write);
  if (ap.second_pass) draw_pass(true, ap.second_colour, ap.second_alpha, ap.second_depth && depth.write);

  const bool colour_written = true;
  const bool depth_written = uses_depth && depth.write;
  if (colour_written) s.colour.dirty = unite(s.colour.dirty, area);
  if (depth_written) s.depth.dirty = unite(s.depth.dirty, area);
  if (uses_depth) ++stats_.depth_batches;
  stats_.batches++;
  stats_.draws++;
  stats_.vertices += verts.size();
  return true;
#endif
}

} // namespace hwgs::dk
