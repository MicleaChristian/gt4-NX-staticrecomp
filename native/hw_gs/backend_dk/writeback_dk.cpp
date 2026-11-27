#include "writeback_dk.hpp"

#include "hw_gs_pixel.hpp"
#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"
#include "platform/nx/deko3d_present.hpp"
#include "platform/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__SWITCH__)
#include <deko3d.h>
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
#include <malloc.h> // memalign

#include "gs_draw_fsh_dksh.hpp"
#include "gs_draw_vsh_dksh.hpp"
#endif

namespace hwgs::dk {
namespace {

// NX-2026-11-27-AH: wall-time accumulators for the "[hw-dk] time" report (one line per 60 frames).
// draw is all of draw_batch; flush and cover include the waits they trigger; wait is every
// submit_and_wait (queue drain) wherever it was called from.
// NX-2026-11-27-AO: texture-hit write-backs of the window by cause: key = TBP0 | PSM << 16 |
// first hit slot << 24. Printed (top 5) with the time line.
std::unordered_map<std::uint32_t, std::uint32_t> g_hit_hist;

// NX-2026-11-27-AY: causes of the three costly surface operations, per report window. Key =
// surface base | FBW << 16 | PSM << 24 | extra << 32 | flag << 48. Printed (top 6 by count) with
// the time line.
//   g_cover_hist: seeds from SLM; extra = rows of the requested area, flag = depth surface.
//   g_flush_hist: write-backs forced by a memory access; extra = first block, flag = write.
//   g_trim_hist:  `seeded` cut back; extra = first written block, flag = cut by a write-back of
//                 another slot (clear = by an upload).
struct HistVal {
  std::uint32_t n = 0;
  std::uint64_t bytes = 0;
};
std::unordered_map<std::uint64_t, HistVal> g_cover_hist, g_flush_hist, g_trim_hist;
int g_trim_from = 0;
// Texture slots given to another texture, per size class (see kTexClasses).
std::uint32_t g_tex_evict[8] = {};

void hist_add(std::unordered_map<std::uint64_t, HistVal> &h, std::uint64_t base, std::uint32_t fbw, std::uint32_t psm,
              std::uint64_t extra, bool flag, std::uint64_t bytes) {
  if (h.size() > 512u) return;
  HistVal &v = h[(base & 0xFFFFu) | (static_cast<std::uint64_t>(fbw & 0xFFu) << 16) |
                 (static_cast<std::uint64_t>(psm & 0xFFu) << 24) | ((extra & 0xFFFFu) << 32) |
                 (static_cast<std::uint64_t>(flag ? 1u : 0u) << 48)];
  ++v.n;
  v.bytes += bytes;
}

void hist_print(std::FILE *out, const char *title, std::unordered_map<std::uint64_t, HistVal> &h) {
  if (h.empty()) return;
  std::fprintf(out, "[hw-dk] %s:", title);
  for (int shown = 0; shown < 6 && !h.empty(); ++shown) {
    auto best = h.begin();
    for (auto it = h.begin(); it != h.end(); ++it)
      if (it->second.n > best->second.n) best = it;
    const std::uint64_t k = best->first;
    std::fprintf(out, " %05x/%u/%02x+%04x%s:%u(%lluK)", static_cast<unsigned>(k & 0xFFFFu),
                 static_cast<unsigned>((k >> 16) & 0xFFu), static_cast<unsigned>((k >> 24) & 0xFFu),
                 static_cast<unsigned>((k >> 32) & 0xFFFFu), ((k >> 48) & 1u) ? "*" : "", best->second.n,
                 static_cast<unsigned long long>(best->second.bytes / 1024u));
    h.erase(best);
  }
  std::fprintf(out, "\n");
  h.clear();
}

struct Prof {
  std::uint64_t wait = 0, flush = 0, cover = 0, decode = 0, texup = 0, draw = 0, rb_copy = 0;
  std::uint32_t decodes = 0, texups = 0, revalid = 0;
  std::uint64_t idle_wait = 0;
  std::uint32_t idle_waits = 0;
  // NX-2026-11-27-BB: hook = all of before_memory_access (one call per GS memory transfer),
  // hash = tex_source_hash, hooks / hashes = their call counts.
  std::uint64_t hook = 0, hash = 0;
  std::uint32_t hooks = 0, hashes = 0;
  std::uint32_t hook_skips = 0; // NX-2026-11-27-BC: hook calls answered by State::seen
};
Prof g_prof;

// NX-2026-11-27-BC: totals of LocalMemorySink::CommitUpload (cpuref_memory.cpp): nanoseconds in
// hostToLocal, bytes and calls. Printed as deltas with the time line.
extern "C" {
extern unsigned long long gt4_gs_upload_ns;
extern unsigned long long gt4_gs_upload_bytes;
extern unsigned long long gt4_gs_uploads;
}
unsigned long long g_upload_prev[3] = {0, 0, 0};

// NX-2026-11-27-BE: see cpuref_memory.cpp.
extern "C" {
struct Gt4GsWriteInfo {
  unsigned valid, base, bw, psm, x0, y0, w, h, page_rows, page_w, exact_first, exact_blocks;
};
extern Gt4GsWriteInfo gt4_gs_write_info;
}
std::uint32_t g_write_drops = 0;
std::uint32_t g_superseded = 0; // NX-2026-11-27-BN: pending write-backs dropped under a full cover (per window)
struct ProfScope {
  std::uint64_t &acc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit ProfScope(std::uint64_t &a) : acc(a) {}
  ~ProfScope() {
    acc += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
};

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
  // NX-2026-11-27-AD: holds a hash of the decoded palette (0 for direct-colour textures), not the
  // CLUT load counter. The counter rises on every palette load, even one that loads the same
  // entries again, so no indexed texture ever matched its cached decode (run 14: 4.7 cache hits
  // for 85 draws per frame on car select).
  std::uint32_t clut_gen = 0;
  // NX-2026-11-27-AW: hash of the SLM bytes the texture was decoded from; 0 in an address key
  // (State::tex_meta), set in a content key (State::tex_cache, TexSlot::key).
  std::uint64_t content = 0;
  bool operator==(const TexKey &o) const {
    return tex0 == o.tex0 && texa == o.texa && clut_gen == o.clut_gen && content == o.content;
  }
};

struct TexKeyHash {
  std::size_t operator()(const TexKey &k) const {
    return static_cast<std::size_t>(k.tex0 ^ (k.texa * 0x9E3779B97F4A7C15ull) ^
                                    (static_cast<std::uint64_t>(k.clut_gen) << 17) ^ (k.content * 0xD6E8FEB86659FD93ull));
  }
};

// NX-2026-11-27-AG: fast SLM pixel access. LocalMemory::readRect / writeRect / readPixel compute a
// generic bit-field slot per pixel; on car select the backend moves about 7 million pixels per
// frame through them (surface seeds, write-backs, texture decodes), which was most of
// gs_gpu_draw_ms. The tables below hold, for one GS page of each format, the storage index of
// every pixel. They are measured once from LocalMemory itself (fill a scratch memory with index
// patterns, read every pixel of page 0), so they follow the reference addressing by construction,
// and each one is checked to be a permutation; a failed check leaves `ok` false and every caller
// falls back to the LocalMemory functions.
struct FastTables {
  std::vector<std::uint16_t> ct32; // [32][64]   32-bit word index in the page (CT32, CT24)
  std::vector<std::uint16_t> t8;   // [64][128]  byte index in the page (PSMT8)
  std::vector<std::uint16_t> t4;   // [128][128] nibble index in the page (PSMT4), low nibble first
  bool ok = false;
};

bool is_permutation(const std::vector<std::uint16_t> &v) {
  std::vector<std::uint8_t> seen(v.size(), 0);
  for (std::uint16_t i : v) {
    if (i >= v.size() || seen[i]) return false;
    seen[i] = 1;
  }
  return true;
}

FastTables build_fast_tables() {
  FastTables t;
  auto scratch = std::make_unique<memory::LocalMemory>();
  std::uint8_t *b = scratch->bytes().data();
  constexpr std::uint32_t kPageBytes = 8192u;
  // CT32: word i holds i.
  for (std::uint32_t i = 0; i < kPageBytes / 4u; ++i) std::memcpy(b + i * 4u, &i, 4);
  t.ct32.resize(32u * 64u);
  {
    const memory::BufferView v{0, 1, memory::Psm::Ct32};
    for (std::uint32_t y = 0; y < 32u; ++y)
      for (std::uint32_t x = 0; x < 64u; ++x) t.ct32[y * 64u + x] = static_cast<std::uint16_t>(scratch->readPixel(v, x, y));
  }
  // PSMT8: byte i holds the low, then the high byte of i.
  t.t8.assign(64u * 128u, 0);
  for (int pass = 0; pass < 2; ++pass) {
    for (std::uint32_t i = 0; i < kPageBytes; ++i) b[i] = static_cast<std::uint8_t>(i >> (8 * pass));
    const memory::BufferView v{0, 2, memory::Psm::T8};
    for (std::uint32_t y = 0; y < 64u; ++y)
      for (std::uint32_t x = 0; x < 128u; ++x)
        t.t8[y * 128u + x] |= static_cast<std::uint16_t>((scratch->readPixel(v, x, y) & 0xFFu) << (8 * pass));
  }
  // PSMT4: nibble p (byte p/2, low nibble first) holds 4 bits of p per pass.
  t.t4.assign(128u * 128u, 0);
  for (int pass = 0; pass < 4; ++pass) {
    for (std::uint32_t i = 0; i < kPageBytes; ++i) {
      const std::uint32_t lo = ((2u * i) >> (4 * pass)) & 15u, hi = ((2u * i + 1u) >> (4 * pass)) & 15u;
      b[i] = static_cast<std::uint8_t>(lo | (hi << 4));
    }
    const memory::BufferView v{0, 2, memory::Psm::T4};
    for (std::uint32_t y = 0; y < 128u; ++y)
      for (std::uint32_t x = 0; x < 128u; ++x)
        t.t4[y * 128u + x] |= static_cast<std::uint16_t>((scratch->readPixel(v, x, y) & 15u) << (4 * pass));
  }
  t.ok = is_permutation(t.ct32) && is_permutation(t.t8) && is_permutation(t.t4);
  std::fprintf(stderr, "[hw-dk] fast SLM tables: %s\n", t.ok ? "ok" : "CHECK FAILED, using generic access");
  return t;
}

const FastTables &fast_tables() {
  static const FastTables t = build_fast_tables();
  return t;
}

constexpr std::uint32_t kSlmByteMask = 0x3FFFFFu; // 4 MiB

inline std::uint32_t page_byte_base(std::uint32_t base, std::uint32_t page) {
  return ((base + page * 32u) & 16383u) * 256u;
}

// CT32 / CT24 words of a rectangle, row-major (CT24 callers mask the top byte themselves).
bool fast_read32(const memory::LocalMemory &local, std::uint32_t base, std::uint32_t bw, std::uint32_t x0,
                 std::uint32_t y0, std::uint32_t w, std::uint32_t h, std::uint32_t *out) {
  const FastTables &t = fast_tables();
  if (!t.ok || x0 + w > 2048u || y0 + h > 2048u) return false;
  const std::uint8_t *mem = local.bytes().data();
  const std::uint32_t per_row = std::max<std::uint32_t>(bw, 1u);
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    const std::uint16_t *rt = t.ct32.data() + (y & 31u) * 64u;
    std::uint32_t x = x0;
    while (x < x0 + w) {
      const std::uint32_t pb = page_byte_base(base, (y >> 5) * per_row + (x >> 6));
      const std::uint32_t end = std::min(x0 + w, (x | 63u) + 1u);
      for (; x < end; ++x) {
        std::uint32_t v;
        std::memcpy(&v, mem + ((pb + rt[x & 63u] * 4u) & kSlmByteMask), 4);
        *out++ = v;
      }
    }
  }
  return true;
}

// Stores CT32 words; CT24 stores the low three bytes and leaves the fourth as it is.
bool fast_write32(memory::LocalMemory &local, std::uint32_t base, std::uint32_t bw, std::uint32_t x0, std::uint32_t y0,
                  std::uint32_t w, std::uint32_t h, const std::uint32_t *in, bool ct24) {
  const FastTables &t = fast_tables();
  if (!t.ok || x0 + w > 2048u || y0 + h > 2048u) return false;
  std::uint8_t *mem = local.bytes().data();
  const std::uint32_t per_row = std::max<std::uint32_t>(bw, 1u);
  // NX-2026-11-27-AI: two loops with constant store sizes. A size held in a variable made every
  // pixel a memcpy call (run AH: 13 ns per written-back pixel).
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    const std::uint16_t *rt = t.ct32.data() + (y & 31u) * 64u;
    std::uint32_t x = x0;
    while (x < x0 + w) {
      const std::uint32_t pb = page_byte_base(base, (y >> 5) * per_row + (x >> 6));
      const std::uint32_t end = std::min(x0 + w, (x | 63u) + 1u);
      if (ct24) {
        for (; x < end; ++x) {
          const std::uint32_t v = *in++;
          std::uint8_t *d = mem + ((pb + rt[x & 63u] * 4u) & kSlmByteMask);
          d[0] = static_cast<std::uint8_t>(v);
          d[1] = static_cast<std::uint8_t>(v >> 8);
          d[2] = static_cast<std::uint8_t>(v >> 16);
        }
      } else {
        for (; x < end; ++x) {
          const std::uint32_t v = *in++;
          std::memcpy(mem + ((pb + rt[x & 63u] * 4u) & kSlmByteMask), &v, 4);
        }
      }
    }
  }
  return true;
}

// Palette lookup of a PSMT8 / PSMT4 texture, row-major RGBA.
bool fast_decode_indexed(const memory::LocalMemory &local, std::uint32_t psm_code, std::uint32_t base, std::uint32_t bw,
                         std::uint32_t tw, std::uint32_t th, const std::uint32_t *pal, std::uint32_t pal_count,
                         std::uint32_t *out) {
  const FastTables &t = fast_tables();
  if (!t.ok || tw > 2048u || th > 2048u || (psm_code != 0x13u && psm_code != 0x14u)) return false;
  const std::uint8_t *mem = local.bytes().data();
  const std::uint32_t per_row = std::max<std::uint32_t>(bw * 64u / 128u, 1u);
  const bool t8 = psm_code == 0x13u;
  const std::uint32_t y_shift = t8 ? 6u : 7u, y_mask = t8 ? 63u : 127u;
  const std::uint16_t *table = t8 ? t.t8.data() : t.t4.data();
  for (std::uint32_t y = 0; y < th; ++y) {
    const std::uint16_t *rt = table + (y & y_mask) * 128u;
    std::uint32_t x = 0;
    while (x < tw) {
      const std::uint32_t pb = page_byte_base(base, (y >> y_shift) * per_row + (x >> 7));
      const std::uint32_t end = std::min(tw, (x | 127u) + 1u);
      if (t8) {
        for (; x < end; ++x) {
          const std::uint32_t idx = mem[(pb + rt[x & 127u]) & kSlmByteMask];
          *out++ = idx < pal_count ? pal[idx] : 0u;
        }
      } else {
        for (; x < end; ++x) {
          const std::uint32_t p = rt[x & 127u];
          const std::uint32_t idx = (mem[(pb + (p >> 1)) & kSlmByteMask] >> ((p & 1u) * 4u)) & 15u;
          *out++ = idx < pal_count ? pal[idx] : 0u;
        }
      }
    }
  }
  return true;
}

// NX-2026-11-27-AD: SLM block footprint of a texture by format. FootprintBlocks assumes CT32
// pages (64x32 pixels). PSMT8 pages hold 128x64 texels and PSMT4 pages 128x128, so the CT32 figure
// was 4 and 8 times too large: indexed textures "overlapped" render targets they do not share
// memory with, which forced a readback per draw and dropped unrelated cached decodes.
// `bw` is in 64-pixel units and already covers the texture width.
uint32_t tex_footprint_blocks(uint32_t psm_code, uint32_t bw, uint32_t th) {
  uint32_t page_h = 32u, units_per_page = 1u;
  switch (psm_code) {
    case 0x13u: page_h = 64u; units_per_page = 2u; break;  // PSMT8
    case 0x14u: page_h = 128u; units_per_page = 2u; break; // PSMT4
    case 0x02u:
    case 0x0Au: page_h = 64u; break;                       // CT16 / CT16S
    default: break;                                        // CT32, CT24, 8H, 4HL, 4HH
  }
  const std::uint64_t pages = std::max<uint32_t>((std::max<uint32_t>(bw, 1u) + units_per_page - 1u) / units_per_page, 1u);
  const std::uint64_t rows = std::max<uint32_t>((th + page_h - 1u) / page_h, 1u);
  return static_cast<uint32_t>(std::min<std::uint64_t>(pages * rows * 32u, 16384u));
}

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
  // NX-2026-11-27-AF: draw the batch twice (see map_blend, coefficient Ad).
  bool twice = false;
};

bool map_blend(uint64_t alpha, GpuBlend &out) {
  const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
  const uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFFu);
  auto coef = [&](uint32_t which) {
    return Coef{d == which ? 1 : 0, (a == which ? 1 : 0) - (b == which ? 1 : 0)};
  };
  const Coef ks = coef(0), kd = coef(1);
  // NX-2026-11-27-AF: coefficient Ad (C=1) is mapped to the destination alpha factor. It used to
  // return false here, the caller ignored that and drew the batch with blending off: the car
  // reflection pass (Cs*Ad + Cd, ALPHA 0x58) replaced the painted body with the reflection alone
  // (black / grey car on car select). GS Ad is A/128 while the GPU factor is A/255, about half.
  // For the pure forms Cd + Cs*Ad and Cd - Cs*Ad the batch is drawn twice, which gives A/127.5;
  // other Ad forms stay at half strength (approximate, but blended).
  if ((ks.cf != 0 || kd.cf != 0) && c == 3) return false; // reserved
  const bool fixed = c == 2;
  const bool dest_alpha = c == 1;
  out.twice = dest_alpha && kd.c0 == 1 && kd.cf == 0 && ks.c0 == 0 && (ks.cf == 1 || ks.cf == -1);
  out.constant = std::min(1.f, fix / 128.f);
#if defined(__SWITCH__)
  auto factor = [&](const Coef &k, DkBlendFactor &f) {
    if (k.c0 == 0 && k.cf == 0) f = DkBlendFactor_Zero;
    else if (k.c0 == 1 && k.cf == 0) f = DkBlendFactor_One;
    else if (k.c0 == 0 && k.cf == 1)
      f = fixed ? DkBlendFactor_ConstAlpha : dest_alpha ? DkBlendFactor_DstAlpha : DkBlendFactor_Src1Alpha;
    else if (k.c0 == 1 && k.cf == -1)
      f = fixed ? DkBlendFactor_InvConstAlpha : dest_alpha ? DkBlendFactor_InvDstAlpha : DkBlendFactor_InvSrc1Alpha;
    else return false;
    return true;
  };
  auto negative_f = [](const Coef &k) { return k.c0 == 0 && k.cf == -1; };
  const DkBlendFactor f_pos =
      fixed ? DkBlendFactor_ConstAlpha : dest_alpha ? DkBlendFactor_DstAlpha : DkBlendFactor_Src1Alpha;
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

// Soft gates only: count approximations, but still draw. Hard reject only Z-as-FRAME.
// CT16/CT16S are drawn into the RGBA8 GPU target (approx); menus use them heavily.
bool accept_batch(const hwgs::Batch &batch, bool /*depth_available*/, DkGpuStats &stats) {
  const auto &ctx = batch.state.ctx;
  const uint32_t frame_psm = hwgs::frame::Psm(ctx.frame);
  if (hwgs::IsZPsm(frame_psm)) return reject(stats, Reject::FramePsm);
  if (frame_psm != hwgs::psm::kCt32 && frame_psm != hwgs::psm::kCt24 && frame_psm != hwgs::psm::kCt16 &&
      frame_psm != hwgs::psm::kCt16S) {
    ++stats.rejects[static_cast<int>(Reject::FramePsm)]; // soft: still draw as CT32
  }
  if (hwgs::frame::Fbmsk(ctx.frame) != 0) ++stats.rejects[static_cast<int>(Reject::FrameMask)];
  if (hwgs::test::Date(ctx.test)) ++stats.rejects[static_cast<int>(Reject::DestAlphaTest)];
  if ((batch.state.env.colclamp & 1u) == 0) ++stats.rejects[static_cast<int>(Reject::ColClamp)];
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

// NX-2026-11-27-V: log the first textured batches that get skipped, with the texture that caused it.
void log_tex_reject(const char *why, uint64_t tex0, uint32_t tw, uint32_t th) {
  static int logged = 0;
  if (logged >= 40) return;
  ++logged;
  std::fprintf(stderr, "[hw-dk] T-tex reject: %s psm=0x%x tbp=0x%x tbw=%u tw=%u th=%u cpsm=0x%x\n", why,
               static_cast<unsigned>(hwgs::tex0::Psm(tex0)), static_cast<unsigned>(hwgs::tex0::Tbp0(tex0)),
               static_cast<unsigned>(hwgs::tex0::Tbw(tex0)), tw, th,
               static_cast<unsigned>(hwgs::tex0::Cpsm(tex0)));
}

bool decode_texture(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                    std::vector<std::uint32_t> &out, uint32_t &tw, uint32_t &th) {
  ProfScope prof_decode(g_prof.decode);
  ++g_prof.decodes;
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
    // NX-2026-11-27-AG: table-driven path for PSMT8 / PSMT4; 8H / 4HL / 4HH keep the generic one.
    if (!fast_decode_indexed(local, psm_code, hwgs::tex0::Tbp0(tex0), hwgs::tex0::Tbw(tex0), tw, th, pal.entries,
                             pal.count, out.data())) {
      for (uint32_t y = 0; y < th; ++y) {
        for (uint32_t x = 0; x < tw; ++x) {
          const uint32_t idx = local.readPixel(view, x, y) & 0xFFu;
          out[static_cast<std::size_t>(y) * tw + x] = (idx < pal.count) ? pal.entries[idx] : 0;
        }
      }
    }
  } else {
    const bool words32 = psm_code == hwgs::psm::kCt32 || psm_code == hwgs::psm::kCt24;
    if (!(words32 && fast_read32(local, hwgs::tex0::Tbp0(tex0), hwgs::tex0::Tbw(tex0), 0, 0, tw, th, out.data())))
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
  float st_scale_s = 1.f, st_scale_t = 1.f; // NX-2026-11-27-T: STQ texture smaller than the sampled image
  float y_sign = 1.f; // +1 puts GS row 0 at NDC y = -1 (upper-left origin); the self-test picks the sign
  // NX-2026-11-27-BE: vertex alpha rewrite for a directly sampled CT24 target whose texel alpha is
  // the constant TEXA.TA0 (AEM = 0). The shader runs such a batch with TCC = 0 (A = Av); the GS
  // result with TCC = 1 is put into Av here: 1 MODULATE Av * k >> 7, 2 DECAL / HIGHLIGHT2 k,
  // 3 HIGHLIGHT Av + k. 0 = leave alone.
  int alpha_op = 0;
  uint32_t alpha_k = 0;
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
  if (vs.alpha_op != 0) {
    const uint32_t a = c.a;
    const uint32_t r = vs.alpha_op == 1 ? (a * vs.alpha_k) >> 7 : vs.alpha_op == 3 ? a + vs.alpha_k : vs.alpha_k;
    o.a = static_cast<float>(std::min<uint32_t>(r, 255u)) / 255.f;
  }
  o.fog = v.fog / 255.f;
  if (!vs.tme) {
    o.s = o.t = 0.5f;
    o.q = 1.f;
  } else if (vs.fst) {
    o.s = (static_cast<float>(v.u) / 16.f) / vs.tex_w;
    o.t = (static_cast<float>(v.v) / 16.f) / vs.tex_h;
    o.q = 1.f;
  } else {
    o.s = v.s * vs.st_scale_s;
    o.t = v.t * vs.st_scale_t;
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
// Switch heap + nvmap are tight (especially in applet mode): keep this small.
// NX-2026-11-13-E (reversible): GT4 samples through 1024×512; raise slot from 512×512.
// Remaining TME rejects skip the batch (no 1×1 white soft-draw). Release deko3d aborts
// the process on a failed dkMemBlockCreate.
// NX-2026-11-27-AF: 16 -> 72 slots in less memory: 8 large (4 MiB, up to 1024x1024) and 64 small
// (256 KiB, up to 256x256), 48 MiB instead of 64. Car select draws about 280 batches per frame
// from far more than 16 textures, so every texture was copied into a slot again each frame even
// when its decode was cached, and the queue was drained whenever all 16 were in use (run AE: 34
// waits and 45 MiB of uploads per frame).
// NX-2026-11-27-AY: 72 -> 453 slots in the same 48 MiB, in five size classes. Slots are keyed by
// content since AW, so a texture the game streams again each frame keeps its slot as long as the
// slot is not given away; with 64 small slots the race (about 200 texture and palette pairs per
// frame) and car select gave every slot away each frame: run AX decoded and uploaded 12363
// textures per 60 frames, nearly all with unchanged source bytes (revalidated=12584). A texture
// uses the smallest class it fits; the slot given away is the least recently used of that class.
struct TexClass {
  uint32_t bytes;
  int count;
};
// NX-2026-11-27-AZ: run AY re-used 444 slots of the 1 MiB class per window in the race and none
// of the two small classes, so slots moved from the small classes to 1 MiB and a new 512 KiB
// class (512x256): 3 / 14 / 24 / 32 / 48 / 64 slots, 50 MiB.
// NX-2026-11-27-BA: the AY counts AZ was sized from came from the pre-race preview. In the race
// itself run AZ re-used, per 60 frames, up to 2291 slots of 256 KiB, 3873 of 64 KiB and 6171 of
// 16 KiB (38, 65 and 103 per frame) and none of 1 MiB: 3 / 8 / 8 / 72 / 128 / 192 slots, 53 MiB.
// NX-2026-11-27-BB: run BA still re-used 9193 slots of 16 KiB per 60 frames in the race (about
// 150 per frame beyond the 192 held): 3 / 6 / 8 / 64 / 160 / 768 slots, 60 MiB.
constexpr TexClass kTexClasses[] = {{1024u * 1024u * 4u, 3},
                                    {512u * 512u * 4u, 6},
                                    {512u * 256u * 4u, 8},
                                    {256u * 256u * 4u, 64},
                                    {128u * 128u * 4u, 160},
                                    {64u * 64u * 4u, 768}};
constexpr int kTexClassCount = 6;
constexpr int tex_class_lo(int c) {
  int lo = 0;
  for (int i = 0; i < c; ++i) lo += kTexClasses[i].count;
  return lo;
}
constexpr uint32_t tex_class_mem(int c) {
  uint32_t off = 0;
  for (int i = 0; i < c; ++i) off += kTexClasses[i].bytes * static_cast<uint32_t>(kTexClasses[i].count);
  return off;
}
constexpr int kTexSlots = tex_class_lo(kTexClassCount); // NX-2026-11-27-Q: 4 -> 16 (car select thrashed 4 slots; 32 MiB total)
// NX-2026-11-27-V: 1024x512 -> 1024x1024. TW=TH=10 textures (car select) hit the pitch-overflow
// reject below and the whole textured batch was skipped (black car). 16 x 4 MiB = 64 MiB.
constexpr uint32_t kTexSlotBytes = 1024u * 1024u * 4u; // max 1024x1024 RGBA8 per slot
constexpr int kWhiteSlot = kTexSlots;                 // 1x1 white for untextured batches only
// NX-2026-11-27-T: + one descriptor per colour surface, so a batch can sample a render target directly.
constexpr int kRttDescBase = kTexSlots + 1;
// NX-2026-11-13-J: park last FRAME-tagged GPU colour image so try_scanout can still
// same_target(DISPFB) after draws retarget to FRAME@0 (owner I: steady sub=base).
// NX-2026-11-27-AA: 2 -> 4. Car select cycles five to seven render targets per frame; with two
// slots every retarget evicted (and flushed) a target that was drawn again a few batches later.
// Victim choice is least recently used (Surface::stamp).
// NX-2026-11-27-AB: 4 -> 12. Run 13 showed nine distinct targets (eleven with the fbw=2 aliases)
// used in a fixed cycle each frame; a cyclic pattern is the worst case for LRU, so four slots still
// evicted on nearly every retarget (victim_flushes=79 per window, ~480 MiB of re-seeding uploads).
// NX-2026-11-27-AJ: one GPU depth surface per Z target (base, width, format), 3 kept. With a
// single surface every change of Z target cleared it, and car select changes Z target in the
// middle of the car (the 320-wide effect targets use the same ZBP with FBW 5): geometry drawn
// after the switch back no longer saw the depth of what was drawn before (mirror behind the body).
// NX-2026-11-27-BT: was 3. The race uses more depth targets than that in one frame (the main Z
// buffer at 0x8c0 or 0, plus the Z buffers of the small targets: 0x3e40, 0x1c40, and the main
// addresses again at widths 4 and 1), and a target that loses its slot loses its depth (depth is
// never written back). When the main Z buffer was the one evicted between the track and the cars,
// the cars were drawn over everything (runs BM and BS). 4 MiB per slot.
// NX-2026-11-27-BV: 8 was still short. Run BT: about 16 depth targets are in use around car select
// and the race start, and the main Z buffer (`008c0/10/31`, `00000/10/31`, fully drawn) was evicted
// for a 128-wide one. 16 slots, and see the eviction order below.
constexpr int kDepthSlots = 16;
constexpr int kColourSlots = 12;
// NX-2026-11-27-AU: + one descriptor for the self-sample snapshot image.
constexpr int kSnapDesc = kTexSlots + 1 + kColourSlots;
constexpr int kImageDescriptors = kTexSlots + 1 + kColourSlots + 1;
constexpr bool kDirectRtt = true;
// Sampler index = linear * 4 + clamp S * 2 + clamp T (clamp = CLAMP or REGION_CLAMP).
constexpr int kSamplers = 8;
// Image and sampler descriptors share one 0x1000-byte memblock (see init).
// NX-2026-11-27-AY: kDescBytes (0x6000) now, for 453 texture slots.
constexpr uint32_t kDescBytes = 0xA000u; // NX-2026-11-27-BB: 1009 texture slots
static_assert(kImageDescriptors * sizeof(DkImageDescriptor) + DK_SAMPLER_DESCRIPTOR_ALIGNMENT +
                  kSamplers * sizeof(DkSamplerDescriptor) <= kDescBytes,
              "descriptor memblock too small for kColourSlots");
constexpr uint32_t kParamsBytes = DK_UNIFORM_BUF_ALIGNMENT;
// NX-2026-11-27-AZ: operations per command list before the queue is drained: 4 MiB of command
// memory / 2500 = 1.6 KiB per operation, several times what one draw records.
constexpr uint32_t kMaxListOps = 2500u;
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
  std::uint32_t last_use = 0; // NX-2026-11-27-AY: State::tex_clock at the last draw that used it
  bool stale = false; // NX-2026-11-27-AV: SLM under the texture was written since the upload
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
  std::uint64_t stamp = 0;      // State::use_clock value of the last draw or direct sample (LRU)
  Rect seeded;                  // area whose GPU copy equals SLM (or holds newer pixels inside `dirty`)
  Rect dirty;                   // area drawn on the GPU and not yet written back
  std::uint64_t dirty_stamp = 0; // NX-2026-11-27-AJ: use_clock of the last draw that added to `dirty`

  bool same_target(const memory::BufferView &v) const {
    return valid && view.basePointer == v.basePointer && view.bufferWidth == v.bufferWidth && view.psm == v.psm;
  }
  // NX-2026-11-27-AD: pixel rows per GS page (16-bit formats: 64, others: 32) and the number of
  // blocks that lie before the page row holding pixel row y. Pages are linear in memory, so rows
  // above y0 of a rectangle are not part of its block range.
  uint32_t rows_per_page() const {
    return (psm_code == 0x02u || psm_code == 0x0Au || psm_code == 0x32u || psm_code == 0x3Au) ? 64u : 32u;
  }
  uint32_t row_block_offset(int y) const {
    return (static_cast<uint32_t>(std::max(y, 0)) / rows_per_page()) * std::max<uint32_t>(fbw, 1u) * 32u;
  }
  // NX-2026-11-27-AN: blocks of the first `rows` pixel rows. FootprintBlocks counts 32-row pages;
  // a 16-bit page holds 64 rows, so 16-bit targets were counted at twice their size and "overlapped"
  // textures stored right after them (race: one write-back per HUD and scenery draw).
  uint32_t rows_blocks(int rows) const {
    const uint32_t r = static_cast<uint32_t>(std::max(rows, 0));
    return hwgs::FootprintBlocks(fbw, rows_per_page() == 64u ? (r + 1u) / 2u : r);
  }
  // Block range [first, first + count) of pixel rows y0..y1 (conservative).
  void row_range(int y0, int y1, uint32_t &first, uint32_t &count) const {
    const uint32_t off = row_block_offset(y0);
    const uint32_t end = rows_blocks(y1 + 1);
    first = static_cast<uint32_t>(view.basePointer) + off;
    count = end > off ? end - off : 0u;
  }
  // Conservative block range of the rows the GPU copy holds.
  bool overlaps(uint32_t block, uint32_t blocks) const {
    const Rect held = unite(seeded, dirty);
    if (!valid || held.empty()) return false;
    uint32_t first = 0, count = 0;
    row_range(held.y0, held.y1, first, count);
    return hwgs::BlockRangesOverlap(block, blocks, first, count);
  }
  bool dirty_overlaps(uint32_t block, uint32_t blocks) const {
    if (!valid || dirty.empty()) return false;
    uint32_t first = 0, count = 0;
    row_range(dirty.y0, dirty.y1, first, count);
    return hwgs::BlockRangesOverlap(block, blocks, first, count);
  }
};

// NX-2026-11-27-AD: SLM blocks [block, block + blocks) were rewritten by a flush of another slot.
// Keep the seeded rows of `o` that lie wholly above or wholly below the rewritten page rows (the
// larger of the two parts), as long as the kept part still contains `dirty`. Before this, any
// flush under a slot's conservative footprint cut `seeded` down to `dirty`, and the next draw
// re-read and re-uploaded the whole surface from SLM (run 14: 761 seeds and 1875 MiB of uploads
// per 60 frames on car select).
// NX-2026-11-27-AP: the race 3D buffer (strip hunt).
// NX-2026-11-27-AY: true when the sprites of a batch, side by side or stacked, cover every pixel
// of `area`. GS sprite coverage: pixel p is inside when lo <= p * 16 < hi (12.4 coordinates), so
// the first covered pixel is ceil(lo / 16) and the last is ceil(hi / 16) - 1.
bool sprites_tile(const hwgs::Batch &batch, const Rect &area) {
  const auto &v = batch.vertices;
  const std::size_t n = v.size() / 2u;
  if (n < 2u || n > 64u || (v.size() & 1u)) return false;
  auto first_px = [](int32_t c4) { return (c4 + 15) >> 4; };
  auto last_px = [](int32_t c4) { return ((c4 + 15) >> 4) - 1; };
  for (int axis = 0; axis < 2; ++axis) { // 0: columns side by side, 1: rows stacked
    const int a0 = axis == 0 ? area.x0 : area.y0, a1 = axis == 0 ? area.x1 : area.y1;
    const int o0 = axis == 0 ? area.y0 : area.x0, o1 = axis == 0 ? area.y1 : area.x1;
    std::pair<int32_t, int32_t> spans[64];
    bool ok = true;
    for (std::size_t i = 0; i < n && ok; ++i) {
      const auto &p = v[i * 2u], &q = v[i * 2u + 1u];
      const int32_t al = axis == 0 ? std::min(p.x4, q.x4) : std::min(p.y4, q.y4);
      const int32_t ah = axis == 0 ? std::max(p.x4, q.x4) : std::max(p.y4, q.y4);
      const int32_t ol = axis == 0 ? std::min(p.y4, q.y4) : std::min(p.x4, q.x4);
      const int32_t oh = axis == 0 ? std::max(p.y4, q.y4) : std::max(p.x4, q.x4);
      if (first_px(ol) > o0 || last_px(oh) < o1) ok = false;
      spans[i] = {al, ah};
    }
    if (!ok) continue;
    std::sort(spans, spans + n);
    if (first_px(spans[0].first) > a0) continue;
    int32_t reach = spans[0].second;
    for (std::size_t i = 1; i < n && ok; ++i) {
      if (spans[i].first > reach) ok = false;
      reach = std::max(reach, spans[i].second);
    }
    if (ok && last_px(reach) >= a1) return true;
  }
  return false;
}

bool is_race3d(const Surface &c) {
  return c.valid && !c.is_depth && c.view.basePointer == 0x1a40u && c.fbw == 10u && c.psm_code == 0u;
}
bool g_race_trace = false; // set once the 640x448 screen copy has been seen

// NX-2026-11-27-BE: true when the write being announced replaces every unflushed pixel of `c`, so
// the surface need not be written back before it. Two cases. Same buffer (base, width and page
// shape): the written pixel rectangle contains the dirty rectangle. Any other layout: the write
// is whole pages and their block range contains the block range of the dirty rows. Formats that
// store only part of a pixel never qualify (8H / 4HL / 4HH write the top byte only; a 24-bit
// write keeps the alpha byte and qualifies only over a 24-bit surface).
bool write_covers_dirty(const Surface &c) {
  const Gt4GsWriteInfo &w = gt4_gs_write_info;
  if (!w.valid || c.dirty.empty()) return false;
  const bool full_pixel = w.psm == 0x00u || w.psm == 0x02u || w.psm == 0x0Au;
  const bool ct24_pair = w.psm == 0x01u && c.psm_code == 0x01u;
  if (c.view.basePointer == w.base && c.fbw == w.bw && w.page_w == 64u && c.rows_per_page() == w.page_rows &&
      (full_pixel || ct24_pair)) {
    return static_cast<int>(w.x0) <= c.dirty.x0 && static_cast<int>(w.x0 + w.w) > c.dirty.x1 &&
           static_cast<int>(w.y0) <= c.dirty.y0 && static_cast<int>(w.y0 + w.h) > c.dirty.y1;
  }
  if (w.exact_blocks != 0u && (full_pixel || w.psm == 0x13u || w.psm == 0x14u)) {
    uint32_t first = 0, count = 0;
    c.row_range(c.dirty.y0, c.dirty.y1, first, count);
    return count != 0u && first >= w.exact_first &&
           static_cast<std::uint64_t>(first) + count <= static_cast<std::uint64_t>(w.exact_first) + w.exact_blocks;
  }
  return false;
}

void trim_seeded_impl(Surface &o, uint32_t block, uint32_t blocks);
void trim_seeded(Surface &o, uint32_t block, uint32_t blocks) {
  const Rect before = o.seeded;
  trim_seeded_impl(o, block, blocks);
  {
    auto px = [](const Rect &r) {
      return r.empty() ? 0ll : static_cast<long long>(r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
    };
    const long long lost = px(before) - px(o.seeded);
    if (lost > 0)
      hist_add(g_trim_hist, o.view.basePointer, o.fbw, o.psm_code, block, g_trim_from != 0,
               static_cast<std::uint64_t>(lost) * 4u);
  }
  static unsigned n = 0;
  if (g_race_trace && is_race3d(o) && n < 80) {
    ++n;
    std::fprintf(stderr,
                 "[hw-dk] R3D trim #%u: write block=%05x blocks=%u | seeded (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d) | dirty "
                 "(%d,%d)-(%d,%d)\n",
                 n, block, blocks, before.x0, before.y0, before.x1, before.y1, o.seeded.x0, o.seeded.y0, o.seeded.x1,
                 o.seeded.y1, o.dirty.x0, o.dirty.y0, o.dirty.x1, o.dirty.y1);
  }
}

void trim_seeded_impl(Surface &o, uint32_t block, uint32_t blocks) {
  const Rect fallback = intersect(o.seeded, o.dirty);
  const std::uint64_t base = o.view.basePointer;
  const std::uint64_t end = static_cast<std::uint64_t>(block) + blocks;
  if (o.seeded.empty() || end > 16384u || end <= base) {
    o.seeded = fallback;
    return;
  }
  const std::uint64_t row_blocks = static_cast<std::uint64_t>(std::max<uint32_t>(o.fbw, 1u)) * 32u;
  const std::uint64_t rpp = o.rows_per_page();
  const int first = block > base ? static_cast<int>(std::min<std::uint64_t>(((block - base) / row_blocks) * rpp, 1u << 20)) : 0;
  const int last = static_cast<int>(std::min<std::uint64_t>(((end - base + row_blocks - 1u) / row_blocks) * rpp, 1u << 20)) - 1;
  const Rect top = intersect(o.seeded, Rect{0, 0, 1 << 20, first - 1});
  const Rect bottom = intersect(o.seeded, Rect{0, last + 1, 1 << 20, 1 << 20});
  auto rows = [](const Rect &r) { return r.empty() ? 0 : r.y1 - r.y0 + 1; };
  const Rect keep = rows(top) >= rows(bottom) ? top : bottom;
  if (!keep.empty() && (o.dirty.empty() || keep.contains(o.dirty))) {
    o.seeded = keep;
  } else {
    o.seeded = fallback;
  }
}

constexpr unsigned kColourBit = 1u, kDepthBit = 2u;
constexpr unsigned kSlotBit(int slot) { return 4u << slot; }

} // namespace

struct WritebackDk::State {
  DkDevice device = nullptr;
  DkQueue queue = nullptr;
  DkCmdBuf cmdbuf = nullptr;
  DkMemBlock cmd_mem = nullptr;
  DkMemBlock code_mem = nullptr;
  DkMemBlock data_mem = nullptr;
  DkMemBlock stage_mem = nullptr;
  // NX-2026-11-27-AH: GPU -> CPU readback target, CPU-cached. The stage block is CPU-uncached
  // (right for uploads, which only write). Reading a readback out of uncached memory word by
  // word costs a DRAM access per load; run AG spent about 12 ms per write-back that way, ten
  // times per frame on car select. The CPU never writes this block, and its cache lines are
  // dropped (dkMemBlockFlushCpuCache) after each GPU copy, before the CPU reads.
  DkMemBlock rb_mem = nullptr;
  std::uint8_t *rb_cpu = nullptr;
  void *owned_rb = nullptr;
  DkMemBlock desc_mem = nullptr;
  DkMemBlock tex_mem = nullptr;
  DkMemBlock ubo_mem = nullptr;
  DkShader vshader{};
  DkShader fshader{};
  Surface colour[kColourSlots];
  int colour_active = 0; // slot bound for draws
  // NX-2026-11-27-AM: the two most recent display targets (base block, FBW). Their slots are not
  // chosen as eviction victims while another slot can be taken. The race uses more than 12 render
  // targets per frame; plain LRU evicted the display buffer every frame, so each present missed
  // the GPU scanout and fell back to composing 640x448 on the CPU (gs_compose_ms 24.68).
  std::uint32_t display_base[2] = {~0u, ~0u};
  std::uint32_t display_fbw[2] = {0, 0};
  unsigned display_n = 0;
  bool is_display_target(const Surface &c) const {
    for (int k = 0; k < 2; ++k) {
      if (c.valid && static_cast<std::uint32_t>(c.view.basePointer) == display_base[k] && c.fbw == display_fbw[k])
        return true;
    }
    return false;
  }
  std::uint64_t use_clock = 0; // NX-2026-11-27-AA: LRU clock for colour slots
  Surface depth_slots[kDepthSlots];
  int depth_active = 0;
  Surface &dz() { return depth_slots[depth_active]; }
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
  uint32_t stage_used = 0; // NX-2026-11-27-R: bytes of stage queued for SLM->GPU seed copies
  bool recording = false;
  bool descriptors_bound = false;
  int next_tex_slot = 0;
  int next_small_slot = 0; // NX-2026-11-27-AF: round robin inside the small class (unused since AY)
  // NX-2026-11-27-AY: content key -> slot index, and the use counter for least-recently-used.
  std::unordered_map<TexKey, int, TexKeyHash> slot_of;
  std::uint32_t tex_clock = 0;
  TexSlot tex_slots[kTexSlots]{};
  TexSlot white{};
  std::unordered_map<TexKey, std::vector<std::uint32_t>, TexKeyHash> tex_cache;
  // NX-2026-11-27-AV: per cached decode, a hash of the SLM bytes it was decoded from and a "stale"
  // mark. A write under the texture marks it stale instead of dropping it; the next use hashes the
  // source bytes again and keeps the decode (and its GPU slot) when they are unchanged. GT4 uploads
  // the same textures into GS memory every frame: on a still pre-race screen every one of 60
  // textures per frame was decoded and uploaded again (run AU: decode 19 ms and texup 4 ms per frame).
  struct TexMeta {
    std::uint64_t src_hash = 0;
    bool stale = false;
    std::uint32_t clock = 0; // NX-2026-11-27-BB: State::write_clock when src_hash was taken
  };
  std::unordered_map<TexKey, TexMeta, TexKeyHash> tex_meta;
  // NX-2026-11-27-BB: per GS block, the value of write_clock at its last write (upload or
  // write-back). A texture is stale when a block of its footprint carries a stamp newer than
  // TexMeta::clock. Before this every write walked all of tex_meta (up to 8192 entries, each with
  // a footprint computation) to set `stale`: about 300 transfers per frame in the race.
  std::vector<std::uint32_t> block_gen = std::vector<std::uint32_t>(16384u, 0u);
  std::uint32_t write_clock = 1;
  // NX-2026-11-27-BC: memory accesses already handled since the last draw or seed. The hook's work
  // (write back dirty surfaces under the range, cut `seeded`, stamp the blocks) leaves nothing to
  // do for a second access inside a handled range until a draw or a seed changes a surface or a
  // texture is hashed, and both of those raise draw_serial. Run BB: about 2000 hook calls per
  // frame in the race (one per chunk of every transfer and one per palette load), 7 ms per frame.
  struct SeenAccess {
    std::uint32_t block = 0, blocks = 0;
    bool write = false;
  };
  SeenAccess seen[16];
  int seen_n = 0, seen_next = 0;
  std::uint32_t seen_serial = 0;
  std::uint32_t draw_serial = 1;
  std::uint64_t tex_cache_bytes = 0;
  // NX-2026-11-27-T: SLM block ranges written by flush_surfaces since the last targeted
  // invalidation (only textures over these ranges go stale; the rest of the cache survives).
  std::vector<std::pair<uint32_t, uint32_t>> flushed_ranges;
  // CPU backing for memblocks created with maker.storage (we free these; deko3d does not).
  void *owned_cmd = nullptr;
  void *owned_code = nullptr;
  void *owned_colour[kColourSlots] = {};
  // NX-2026-11-27-AU: scratch image for a batch that samples the target it draws into. The GPU
  // cannot sample the bound render target, so the texels the batch can read are copied here
  // (image to image, on the GPU) and sampled from the copy. Before this such a batch wrote the
  // target back to SLM, decoded it and uploaded it as a texture (race: 20 per frame).
  Surface snap;
  void *owned_snap = nullptr;
  void *owned_depth[kDepthSlots] = {};
  void *owned_tex = nullptr;
  void *owned_data = nullptr;
  void *owned_stage = nullptr;
  void *owned_ubo = nullptr;
  void *owned_desc = nullptr;

  Surface &active_colour() { return colour[colour_active]; }
  const Surface &active_colour() const { return colour[colour_active]; }

  bool in_flight = false;  // a submitted list may still be running
  bool gpu_error = false;  // the queue faulted: no further GPU work
  // NX-2026-11-27-AZ: operations (draws, seeds, copies) recorded in the open command list, and
  // the largest value seen in the report window. The command memory is one fixed block and
  // deko3d aborts the process when a list outgrows it; nothing checked this. Until AY the queue
  // was drained many times per frame (texture slots ran out, write-backs), which kept lists
  // short by accident. AY removed most of those drains and the race crashed on its first frame.
  // draw_batch now drains the queue when a list reaches kMaxListOps.
  uint32_t list_ops = 0;
  uint32_t max_list_ops = 0;
  void begin_recording() {
    if (!recording) {
      list_ops = 0;
      // dkCmdBufClear rewinds the command memory, which a running list may still be reading.
      if (in_flight) {
        // NX-2026-11-27-AV: this wait was in no counter. Timed and counted as `idle_waits`.
        ProfScope prof_idle(g_prof.idle_wait);
        ++g_prof.idle_waits;
        dkQueueWaitIdle(queue);
        in_flight = false;
      }
      stage_used = 0; // no list is running: the stage is free again
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
    ++stats.waits;
    ProfScope prof_wait(g_prof.wait);
    submit(stats);
    dkQueueWaitIdle(queue);
    in_flight = false;
    if (!gpu_error && dkQueueIsInErrorState(queue)) {
      gpu_error = true;
      std::fprintf(stderr, "[hw-dk] GPU queue error after %llu submissions: GPU drawer off, CPU draws the rest\n",
                   static_cast<unsigned long long>(stats.submissions));
    }
    vert_offset = 0;
    stage_used = 0;
    for (auto &slot : tex_slots) slot.used_since_wait = false;
  }
};

namespace {

using State = WritebackDk::State;

// Writes the dirty parts of the selected surfaces back into SLM. One surface at a time so the
// staging buffer can stay a single 1024x1024 RGBA/float page (~4 MiB) instead of two.
// NX-2026-11-13-J: kColourBit walks both colour slots (active + parked).
// NX-2026-11-13-K: depth is GPU-local only — never CopyImageToBuffer ZF32 (A crash class).
bool flush_surfaces(State &s, DkGpuStats &stats, memory::LocalMemory &local, unsigned mask, std::string &error) {
  ProfScope prof_flush(g_prof.flush);
  Surface *list[kColourSlots + 1] = {};
  int n = 0;
  // NX-2026-11-27-Z: kSlotBit(i) selects one colour slot (retarget eviction flushes only the
  // slot being overwritten, not both).
  bool sel[kColourSlots] = {};
  for (int i = 0; i < kColourSlots; ++i) {
    if ((mask & kColourBit) || (mask & kSlotBit(i))) sel[i] = true;
  }
  // NX-2026-11-27-AJ: write back in draw order. A slot whose unflushed pixels are older than a
  // selected slot's and share SLM with them goes first. The game renders reflection maps into
  // 128-wide targets placed at the base of the back buffer and then redraws the back buffer;
  // writing the back buffer back first and the older scratch target later put the scratch pixels
  // on top in SLM, and the next seed of the display slot showed them (tiles along the top of
  // car select).
  for (bool grew = true; grew;) {
    grew = false;
    for (int i = 0; i < kColourSlots; ++i) {
      const Surface &x = s.colour[i];
      if (!sel[i] || !x.valid || x.dirty.empty()) continue;
      uint32_t xb = 0, xn = 0;
      x.row_range(x.dirty.y0, x.dirty.y1, xb, xn);
      for (int k = 0; k < kColourSlots; ++k) {
        const Surface &y = s.colour[k];
        if (sel[k] || !y.valid || y.dirty.empty() || y.dirty_stamp >= x.dirty_stamp) continue;
        if (y.dirty_overlaps(xb, xn)) {
          sel[k] = true;
          grew = true;
        }
      }
    }
  }
  for (int i = 0; i < kColourSlots; ++i) {
    if (sel[i]) list[n++] = &s.colour[i];
  }
  std::sort(list, list + n, [](const Surface *a, const Surface *b) { return a->dirty_stamp < b->dirty_stamp; });
  if (mask & kDepthBit) list[n++] = &s.dz();
  bool any = false;
  for (int i = 0; i < n; ++i) {
    if (!list[i] || !list[i]->valid || list[i]->dirty.empty()) continue;
    Rect r = intersect(list[i]->dirty, Rect{0, 0, kDkSurface - 1, kDkSurface - 1});
    list[i]->dirty = {};
    if (r.empty()) continue;
    Surface &surf = *list[i];
    // Depth stays on the GPU for occlusion; SyncForDisplay / Backend::flush may request the
    // depth bit but must not ZF32-readback into SLM (hard abort on deko3d).
    if (surf.is_depth) continue;
    any = true;
    const uint32_t rw = static_cast<uint32_t>(r.x1 - r.x0 + 1), rh = static_cast<uint32_t>(r.y1 - r.y0 + 1);
    s.begin_recording();
    s.barrier();
    DkImageView view;
    dkImageViewDefaults(&view, &surf.image);
    DkImageRect rect{static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), 0, rw, rh, 1};
    DkCopyBuf dst{dkMemBlockGetGpuAddr(s.rb_mem), 0, 0};
    dkCmdBufCopyImageToBuffer(s.cmdbuf, &view, &rect, &dst, 0);
    s.submit_and_wait(stats);
    // NX-2026-11-27-AH: drop the CPU cache lines of the range the GPU just wrote.
    dkMemBlockFlushCpuCache(s.rb_mem, 0, (rw * rh * 4u + 0xFFFu) & ~0xFFFu);
    ProfScope prof_rb(g_prof.rb_copy);
    const std::uint32_t *words = reinterpret_cast<const std::uint32_t *>(s.rb_cpu);
    static std::vector<std::uint32_t> ct16_words; // the readback block is never written by the CPU
    if (surf.psm_code == hwgs::psm::kCt16 || surf.psm_code == hwgs::psm::kCt16S) {
      // GPU colour target is RGBA8 (R in low byte). CT16/CT16S SLM needs packed 16-bit stored words.
      ct16_words.resize(static_cast<std::size_t>(rw) * rh);
      for (std::size_t nword = 0; nword < static_cast<std::size_t>(rw) * rh; ++nword) {
        const pixel::Colour c = pixel::frame_word_to_colour(words[nword]);
        ct16_words[nword] = pixel::pack_frame_word(pixel::FrameFormat::Ct16,
                                                   pixel::colour_to_frame_word(pixel::FrameFormat::Ct16, c));
      }
      words = ct16_words.data();
    }
    {
      const bool f32 = surf.psm_code == hwgs::psm::kCt32, f24 = surf.psm_code == hwgs::psm::kCt24;
      if (!((f32 || f24) && fast_write32(local, static_cast<uint32_t>(surf.view.basePointer), surf.fbw,
                                         static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), rw, rh, words, f24)))
        local.writeRect(surf.view, static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), rw, rh, words);
    }
    // NX-2026-11-27-R: SLM under the other colour slot changed; its GPU copy is stale.
    if (!surf.is_depth) {
      // NX-2026-11-27-AD: the write starts at the page row of r.y0, not at the surface base.
      uint32_t wb = 0, wblocks = 0;
      surf.row_range(r.y0, r.y1, wb, wblocks);
      s.flushed_ranges.emplace_back(wb, wblocks);
      for (int k = 0; k < kColourSlots; ++k) {
        Surface &o = s.colour[k];
        if (&o == &surf || !o.valid || o.seeded.empty()) continue;
        if (o.overlaps(wb, wblocks)) {
          g_trim_from = 1;
          trim_seeded(o, wb, wblocks);
          g_trim_from = 0;
        }
      }
    }
    stats.readback_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
    ++stats.flushes;
  }
  if (!any) s.submit(stats);
  (void)error;
  return true;
}

// NX-2026-11-27-T: conservative SLM block footprint of a cached texture.
bool tex_key_overlaps(const TexKey &k, uint32_t block, uint32_t blocks) {
  const uint32_t tw = 1u << std::min<uint32_t>(hwgs::tex0::Tw(k.tex0), 10u);
  const uint32_t th = 1u << std::min<uint32_t>(hwgs::tex0::Th(k.tex0), 10u);
  const uint32_t bw = std::max<uint32_t>(hwgs::tex0::Tbw(k.tex0), (tw + 63u) / 64u);
  return hwgs::BlockRangesOverlap(block, blocks, hwgs::tex0::Tbp0(k.tex0),
                                  tex_footprint_blocks(hwgs::tex0::Psm(k.tex0), bw, th));
}

// NX-2026-11-27-T: drop only textures whose SLM footprint overlaps [block, block+blocks).
// S wiped the whole 12 MiB cache on every flush: car select re-decoded every texture each batch.
// NX-2026-11-27-AV: marks stale instead of dropping (see State::TexMeta).
// NX-2026-11-27-BB: stamps the written blocks (see State::block_gen); tex_written_since is the
// test at the texture's next use.
void invalidate_textures_overlapping(State &s, uint32_t block, uint32_t blocks, DkGpuStats &stats) {
  (void)stats;
  const std::uint32_t stamp = ++s.write_clock;
  const std::uint32_t n = std::min<uint32_t>(blocks, 16384u);
  for (std::uint32_t i = 0; i < n; ++i) s.block_gen[(block + i) & 16383u] = stamp;
}

bool tex_written_since(const State &s, std::uint64_t tex0, std::uint32_t clock) {
  const uint32_t tw = 1u << std::min<uint32_t>(hwgs::tex0::Tw(tex0), 10u);
  const uint32_t th = 1u << std::min<uint32_t>(hwgs::tex0::Th(tex0), 10u);
  const uint32_t bw = std::max<uint32_t>(hwgs::tex0::Tbw(tex0), (tw + 63u) / 64u);
  const uint32_t n = tex_footprint_blocks(hwgs::tex0::Psm(tex0), bw, th);
  const uint32_t base = static_cast<uint32_t>(hwgs::tex0::Tbp0(tex0));
  for (uint32_t i = 0; i < n; ++i) {
    if (s.block_gen[(base + i) & 16383u] > clock) return true;
  }
  return false;
}

// NX-2026-11-27-AV: hash of the SLM bytes in the same block range tex_key_overlaps tests, so any
// write that can change the texture changes the hash (a write next to it can too, which only
// costs a decode).
std::uint64_t tex_source_hash(const memory::LocalMemory &local, std::uint64_t tex0) {
  const uint32_t tw = 1u << std::min<uint32_t>(hwgs::tex0::Tw(tex0), 10u);
  const uint32_t th = 1u << std::min<uint32_t>(hwgs::tex0::Th(tex0), 10u);
  const uint32_t bw = std::max<uint32_t>(hwgs::tex0::Tbw(tex0), (tw + 63u) / 64u);
  const std::uint64_t bytes = static_cast<std::uint64_t>(tex_footprint_blocks(hwgs::tex0::Psm(tex0), bw, th)) * 256u;
  const std::uint8_t *mem = local.bytes().data();
  std::uint64_t at = (static_cast<std::uint64_t>(hwgs::tex0::Tbp0(tex0)) * 256u) & 0x3FFFFFu;
  std::uint64_t left = bytes;
#if defined(__ARM_FEATURE_CRC32)
  // NX-2026-11-27-BJ: two CRC-32C streams over alternate 16-byte halves, done by the CPU's CRC
  // instructions (the two streams do not wait for each other); the pair is the 64-bit hash. The
  // multiply-and-fold hash below cost 3 to 5 ms per race frame for 100 to 300 textures.
  std::uint32_t c0 = 0x9E3779B9u ^ static_cast<std::uint32_t>(bytes), c1 = 0x7F4A7C15u;
  while (left > 0) {
    const std::uint64_t run = std::min<std::uint64_t>(left, 0x400000u - at); // block aligned: multiple of 16
    for (std::uint64_t i = 0; i < run; i += 16u) {
      std::uint64_t w0, w1;
      std::memcpy(&w0, mem + at + i, 8);
      std::memcpy(&w1, mem + at + i + 8u, 8);
      c0 = __crc32cd(c0, w0);
      c1 = __crc32cd(c1, w1);
    }
    left -= run;
    at = 0;
  }
  return (static_cast<std::uint64_t>(c0) << 32) | c1;
#else
  std::uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
  while (left > 0) {
    const std::uint64_t run = std::min<std::uint64_t>(left, 0x400000u - at); // block aligned: multiple of 8
    for (std::uint64_t i = 0; i < run; i += 8u) {
      std::uint64_t w;
      std::memcpy(&w, mem + at + i, 8);
      h = (h ^ w) * 0x9E3779B97F4A7C15ull;
      h ^= h >> 29;
    }
    left -= run;
    at = 0;
  }
  return h;
#endif
}

void invalidate_flushed(State &s, DkGpuStats &stats) {
  for (const auto &r : s.flushed_ranges) invalidate_textures_overlapping(s, r.first, r.second, stats);
  s.flushed_ranges.clear();
}

// Ensure the GPU surface covers `area`.
// NX-2026-11-13-G cleared new colour area to black; with the parked slot losing `seeded` on every
// retarget, that wiped host-uploaded / previously flushed backgrounds (car-select black screen,
// Q failed on device). NX-2026-11-27-R: seed colour from SLM (CPU readRect -> stage ->
// CopyBufferToImage, no WaitIdle). Only R minus seeded is copied, so dirty (subset of seeded) is
// never overwritten. Depth stays clear-to-0 (GS larger = nearer).
void cover_surface(State &s, DkGpuStats &stats, Surface &surf, const Rect &area_in,
                   const memory::LocalMemory &local) {
  ProfScope prof_cover(g_prof.cover);
  const Rect area = intersect(area_in, Rect{0, 0, kDkSurface - 1, kDkSurface - 1});
  if (area.empty() || surf.seeded.contains(area)) return;
  const Rect seed = unite(surf.seeded, area);
  ++s.draw_serial; // NX-2026-11-27-BC: `seeded` grows: an access handled before must be handled again
  {
    static unsigned n = 0;
    if (g_race_trace && is_race3d(surf) && n < 80) {
      ++n;
      std::fprintf(stderr,
                   "[hw-dk] R3D cover #%u: area (%d,%d)-(%d,%d) | seeded (%d,%d)-(%d,%d) | dirty (%d,%d)-(%d,%d) | new "
                   "seed (%d,%d)-(%d,%d)\n",
                   n, area.x0, area.y0, area.x1, area.y1, surf.seeded.x0, surf.seeded.y0, surf.seeded.x1, surf.seeded.y1,
                   surf.dirty.x0, surf.dirty.y0, surf.dirty.x1, surf.dirty.y1, seed.x0, seed.y0, seed.x1, seed.y1);
    }
  }
  s.begin_recording();
  s.list_ops += 4u;
  DkImageView view;
  dkImageViewDefaults(&view, &surf.image);
  std::uint64_t cover_bytes = 0;
  if (surf.is_depth) {
    const uint32_t rw = static_cast<uint32_t>(area.x1 - area.x0 + 1);
    const uint32_t rh = static_cast<uint32_t>(area.y1 - area.y0 + 1);
    // NX-2026-11-27-Z: depth-only bind. dkCmdBufBindRenderTarget(nullptr, &view) passes a
    // one-entry colour array holding a null pointer, which deko3d dereferences (instant crash
    // with depth_on). Zero colour targets is the correct form.
    dkCmdBufBindRenderTargets(s.cmdbuf, nullptr, 0, &view);
    DkViewport vp{0, 0, (float)kDkSurface, (float)kDkSurface, 0, 1};
    DkScissor sc{static_cast<uint32_t>(area.x0), static_cast<uint32_t>(area.y0), rw, rh};
    dkCmdBufSetViewports(s.cmdbuf, 0, &vp, 1);
    dkCmdBufSetScissors(s.cmdbuf, 0, &sc, 1);
    // NX-2026-11-27-Q: GS depth is larger=nearer (GEQUAL/GREATER), so cleared depth must be 0.0
    // (farthest). Bind a depth-write-enabled DS state first so the clear is not masked.
    DkDepthStencilState ds; dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = true; ds.depthWriteEnable = true; ds.depthCompareOp = DkCompareOp_Always;
    dkCmdBufBindDepthStencilState(s.cmdbuf, &ds);
    dkCmdBufClearDepthStencil(s.cmdbuf, true, 0.f, 0, 0);
  } else {
    // Strips of `seed` not yet seeded.
    Rect strips[4];
    int ns = 0;
    const Rect &S = surf.seeded;
    if (S.empty()) {
      strips[ns++] = seed;
    } else {
      if (seed.y0 < S.y0) strips[ns++] = Rect{seed.x0, seed.y0, seed.x1, S.y0 - 1};
      if (seed.y1 > S.y1) strips[ns++] = Rect{seed.x0, S.y1 + 1, seed.x1, seed.y1};
      if (seed.x0 < S.x0) strips[ns++] = Rect{seed.x0, S.y0, S.x0 - 1, S.y1};
      if (seed.x1 > S.x1) strips[ns++] = Rect{S.x1 + 1, S.y0, seed.x1, S.y1};
    }
    const bool ct16 = surf.psm_code == hwgs::psm::kCt16 || surf.psm_code == hwgs::psm::kCt16S;
    const bool ct24 = surf.psm_code == hwgs::psm::kCt24;
    for (int i = 0; i < ns; ++i) {
      const Rect &r = strips[i];
      if (r.empty()) continue;
      const uint32_t rw = static_cast<uint32_t>(r.x1 - r.x0 + 1);
      const uint32_t rh = static_cast<uint32_t>(r.y1 - r.y0 + 1);
      const uint32_t bytes = rw * rh * 4u;
      if (s.stage_used + bytes > kSurfaceBytes) {
        // Queued copies still read the stage: run them before reusing it.
        s.submit_and_wait(stats);
        s.begin_recording();
      }
      // NX-2026-11-27-AY: the stage block is CPU-uncached. The CT16 and CT24 fix-ups below read
      // every word back, which is very slow on such memory, so those formats are prepared in
      // ordinary memory and copied over once.
      auto *stage_words = reinterpret_cast<std::uint32_t *>(s.stage_cpu + s.stage_used);
      static std::vector<std::uint32_t> fix_tmp;
      std::uint32_t *words = stage_words;
      if (ct16 || ct24) {
        fix_tmp.resize(static_cast<std::size_t>(rw) * rh);
        words = fix_tmp.data();
      }
      cover_bytes += bytes;
      if (!((surf.psm_code == hwgs::psm::kCt32 || ct24) &&
            fast_read32(local, static_cast<uint32_t>(surf.view.basePointer), surf.fbw, static_cast<uint32_t>(r.x0),
                        static_cast<uint32_t>(r.y0), rw, rh, words)))
        local.readRect(surf.view, static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), rw, rh, words);
      if (ct16) {
        for (std::size_t n = 0; n < static_cast<std::size_t>(rw) * rh; ++n)
          words[n] = pixel::unpack_frame_word(pixel::FrameFormat::Ct16, words[n]);
      } else if (ct24) {
        for (std::size_t n = 0; n < static_cast<std::size_t>(rw) * rh; ++n) words[n] &= 0x00FFFFFFu;
      }
      if (words != stage_words) std::memcpy(stage_words, words, bytes);
      DkCopyBuf src{dkMemBlockGetGpuAddr(s.stage_mem) + s.stage_used, 0, 0};
      DkImageRect dst{static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0), 0, rw, rh, 1};
      dkCmdBufCopyBufferToImage(s.cmdbuf, &src, &view, &dst, 0);
      s.stage_used += (bytes + 255u) & ~255u;
      stats.upload_bytes += bytes;
    }
  }
  s.barrier();
  ++stats.seeds;
  hist_add(g_cover_hist, surf.view.basePointer, surf.fbw, surf.psm_code,
           static_cast<std::uint64_t>(area.y1 - area.y0 + 1), surf.is_depth, cover_bytes);
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

  // Budget (MiB): colour×2~8 + depth~4 + tex~4 + verts~2 + stage~4 + misc~1 ≈ 23 (NX-J dual colour).
  // Present's 1280x720 swapchain already sits beside this. Release deko3d aborts on a failed
  // dkMemBlockCreate (cbDebug is debug-only), so we pre-allocate heap storage first.
  s.cmd_bytes = 4 * 1024 * 1024; // NX-2026-11-27-AZ: 256 KiB -> 4 MiB (see State::list_ops)
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
  bool colour_ok = true;
  for (int i = 0; i < kColourSlots && colour_ok; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "colour%d", i);
    colour_ok = make_surface_image(s.device, s.colour[i], DkImageFormat_RGBA8_Unorm,
                                   DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, &s.owned_colour[i], error,
                                   name);
  }
  if (colour_ok) {
    std::string snap_error;
    if (!make_surface_image(s.device, s.snap, DkImageFormat_RGBA8_Unorm,
                            DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, &s.owned_snap, snap_error, "snap")) {
      std::fprintf(stderr, "[hw-dk] snapshot image not created (%s): self-sampling keeps the write-back path\n",
                   snap_error.c_str());
      s.snap.mem = nullptr;
    }
  }
  for (int i = 0; i < kDepthSlots && colour_ok; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "depth%d", i);
    colour_ok = make_surface_image(s.device, s.depth_slots[i], DkImageFormat_ZF32, DkImageFlags_UsageRender,
                                   &s.owned_depth[i], error, name);
  }
  if (!colour_ok) {
    shutdown();
    return false;
  }
  s.colour_active = 0;
  std::fprintf(stderr, "[hw-dk] init: colour×%d and depth surfaces created\n", kColourSlots);
  // NX-2026-11-27-AR: says which build wrote a log (run AQ's log could not be told from AP's).
  std::fprintf(stderr, "[hw-dk] backend built " __DATE__ " " __TIME__ " (NX-2026-11-27-CB)\n");
  for (auto &d : s.depth_slots) d.is_depth = true;

  const uint32_t tex_bytes = tex_class_mem(kTexClassCount) + 0x1000u; // last page: the white texel
  s.tex_mem = create_memblock(s.device, tex_bytes,
                              DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                              &s.owned_tex, error, "tex");
  if (!s.tex_mem) {
    shutdown();
    return false;
  }
  s.tex_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.tex_mem));
  for (int c = 0; c < kTexClassCount; ++c) {
    for (int i = 0; i < kTexClasses[c].count; ++i)
      s.tex_slots[tex_class_lo(c) + i].mem_off = tex_class_mem(c) + static_cast<uint32_t>(i) * kTexClasses[c].bytes;
  }
  std::fprintf(stderr, "[hw-dk] init: %d texture slots, %u KiB\n", kTexSlots, tex_class_mem(kTexClassCount) / 1024u);
  s.white.mem_off = tex_class_mem(kTexClassCount);
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

  const uint32_t vert_bytes = 16u * 1024u * 1024u; // NX-2026-11-27-T: was 2 MiB; ring-full forces a GPU wait
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

  s.rb_mem = create_memblock(s.device, kSurfaceBytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached,
                             &s.owned_rb, error, "readback");
  if (!s.rb_mem) {
    shutdown();
    return false;
  }
  s.rb_cpu = static_cast<std::uint8_t *>(dkMemBlockGetCpuAddr(s.rb_mem));

  s.ubo_mem = create_memblock(s.device, kParamsBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                              &s.owned_ubo, error, "ubo");
  if (!s.ubo_mem) {
    shutdown();
    return false;
  }

  s.desc_mem = create_memblock(s.device, kDescBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
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
    for (int i = 0; i < kColourSlots; ++i) {
      DkImageView cv;
      dkImageViewDefaults(&cv, &s.colour[i].image);
      dkImageDescriptorInitialize(&s.image_desc[kRttDescBase + i], &cv, false, false);
    }
    if (s.snap.mem) {
      DkImageView sv;
      dkImageViewDefaults(&sv, &s.snap.image);
      dkImageDescriptorInitialize(&s.image_desc[kSnapDesc], &sv, false, false);
    }
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
    last_report_ = {};
    has_last_report_ = false;
    return;
  }
  State &s = *state_;
  if (s.queue) dkQueueWaitIdle(s.queue);
  if (s.cmdbuf) dkCmdBufDestroy(s.cmdbuf);
  for (DkMemBlock m : {s.cmd_mem, s.desc_mem, s.ubo_mem, s.stage_mem, s.rb_mem, s.data_mem, s.tex_mem,
                       s.code_mem}) {
    if (m) dkMemBlockDestroy(m);
  }
  for (int i = 0; i < kDepthSlots; ++i) {
    if (s.depth_slots[i].mem) dkMemBlockDestroy(s.depth_slots[i].mem);
    if (i == 0) {
      if (s.snap.mem) dkMemBlockDestroy(s.snap.mem);
      std::free(s.owned_snap);
    }
    std::free(s.owned_depth[i]);
  }
  for (int i = 0; i < kColourSlots; ++i) {
    if (s.colour[i].mem) dkMemBlockDestroy(s.colour[i].mem);
    std::free(s.owned_colour[i]);
  }
  for (void *p : {s.owned_cmd, s.owned_code, s.owned_tex, s.owned_data, s.owned_stage, s.owned_rb, s.owned_ubo,
                  s.owned_desc}) {
    std::free(p);
  }
  delete state_;
  state_ = nullptr;
#endif
  handles_ = {};
  stats_ = {};
  last_report_ = {};
  has_last_report_ = false;
}

bool WritebackDk::self_test(std::string &error) {
#if !defined(__SWITCH__)
  error = "not switch";
  return false;
#else
  // Draws known sprites into a scratch local memory and reads them back: picks the NDC y
  // direction for GS row 0. NX-L: depth stays off after colour (K instant crash); ZF32 gated.
  State &s = *state_;
  auto local = std::make_unique<memory::LocalMemory>();
  CounterSet counters;
  clut::ClutUnit clut(counters);
  constexpr uint32_t kFbp = 0, kFbw = 10, kZbp = 200;
  const memory::BufferView frame_view{memory::blockOfPage(kFbp), kFbw, memory::Psm::Ct32};

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
    for (int i = 0; i < kColourSlots; ++i) {
      s.colour[i].valid = false;
      s.colour[i].seeded = s.colour[i].dirty = {};
    }
    s.colour_active = 0;
    for (auto &d : s.depth_slots) {
      d.valid = false;
      d.seeded = d.dirty = {};
    }
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
    std::fflush(stderr);
    if (!draw_batch(make_sprite(0, 0, 8, 8, 0, 255, 0, false), *local, clut, error)) return false;
    flush_to_slm(*local, error);
    std::fprintf(stderr, "[hw-dk] self-test: colour read back, pixel(2,2)=0x%08X queue_error=%d\n",
                 local->readPixel(frame_view, 2, 2), s.gpu_error ? 1 : 0);
    std::fflush(stderr);
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
    last_report_ = {};
    has_last_report_ = false;
    return false;
  }

  // NX-2026-11-13-L: keep depth_available=false after colour self-test (K enabled GPU depth
  // draws → instant Switch crash). Do NOT restore depth sprite self-test (A crash class).
  // Colour flush + dual slots (J) + I diagnostics unchanged; flush_surfaces still skips ZF32.
  // NX-2026-11-27-Q: depth stays OFF by default. Opt-in test only: create the empty file
  // sdmc:/switch/gt4recomp/depth_on to enable GPU depth draws (delete it if the game crashes).
  // NX-2026-11-27-Y: look for depth_on next to the NRO first (any folder name, e.g.
  // sdmc:/switch/GT4/), then fall back to the old fixed sdmc:/switch/gt4recomp/ path.
  s.depth_available = false;
  std::string depth_flag_found;
  {
    const std::string app_flag = (gt4::platform::executable_directory() / "depth_on").string();
    const char *candidates[] = {app_flag.c_str(), "depth_on", "sdmc:/switch/gt4recomp/depth_on"};
    for (const char *path : candidates) {
      if (FILE *f = std::fopen(path, "rb")) {
        std::fclose(f);
        s.depth_available = true;
        depth_flag_found = path;
        break;
      }
    }
    std::fprintf(stderr, "[hw-dk] self-test: depth=%s (opt-in file %s%s)\n", s.depth_available ? "ON" : "off",
                 s.depth_available ? depth_flag_found.c_str() : app_flag.c_str(),
                 s.depth_available ? "" : " not found");
  }
  std::fflush(stderr);
  error.clear();
  reset_surfaces();
  stats_ = {};
  last_report_ = {};
  has_last_report_ = false;
  return true;
#endif
}

void WritebackDk::report(std::FILE *out) const {
  if (!out) return;
  const auto &cur = stats_;
  const auto &prev = last_report_;
  const bool have_delta = has_last_report_;
  auto d64 = [&](std::uint64_t now, std::uint64_t was) -> unsigned long long {
    return static_cast<unsigned long long>(have_delta && now >= was ? now - was : now);
  };
  std::fprintf(out,
               "[hw-dk] GPU batches=%llu (depth %llu) skipped=%llu covers=%llu flushes=%llu "
               "gpu_scanouts=%llu direct_presents=%llu readback_MiB=%.1f upload_MiB=%.1f\n",
               static_cast<unsigned long long>(cur.batches), static_cast<unsigned long long>(cur.depth_batches),
               static_cast<unsigned long long>(cur.cpu_fallback), static_cast<unsigned long long>(cur.seeds),
               static_cast<unsigned long long>(cur.flushes), static_cast<unsigned long long>(cur.gpu_scanouts),
               static_cast<unsigned long long>(cur.direct_presents),
               cur.readback_bytes / 1048576.0, cur.upload_bytes / 1048576.0);
  // Soft approx = still drawn; skip = batch not GPU-drawn. FramePsm mixes hard Z-as-FRAME reject with soft
  // unknown-PSM approx; listed under soft for visibility (hard path also bumps cpu_fallback via caller).
  static constexpr int kSoft[] = {static_cast<int>(Reject::DepthUnavailable), static_cast<int>(Reject::FrameMask),
                                  static_cast<int>(Reject::DestAlphaTest), static_cast<int>(Reject::ColClamp),
                                  static_cast<int>(Reject::FramePsm)};
  static constexpr int kSkip[] = {static_cast<int>(Reject::ZFormat), static_cast<int>(Reject::AlphaFailBlend),
                                  static_cast<int>(Reject::Blend), static_cast<int>(Reject::PrimMix),
                                  static_cast<int>(Reject::Texture), static_cast<int>(Reject::Surface),
                                  static_cast<int>(Reject::Other)};
  auto dump_group = [&](const char *label, const int *ids, int n) {
    std::fprintf(out, "[hw-dk] %s:", label);
    bool any = false;
    for (int i = 0; i < n; ++i) {
      const int id = ids[i];
      const std::uint64_t total = cur.rejects[id];
      if (!total) continue;
      any = true;
      std::fprintf(out, " %s=%llu", kRejectNames[id], static_cast<unsigned long long>(total));
      if (have_delta) {
        const std::uint64_t delta = total >= prev.rejects[id] ? total - prev.rejects[id] : total;
        std::fprintf(out, "(+%llu)", static_cast<unsigned long long>(delta));
      }
    }
    if (!any) std::fprintf(out, " (none)");
    std::fprintf(out, "\n");
  };
  dump_group("soft approx (still drawn)", kSoft, static_cast<int>(sizeof(kSoft) / sizeof(kSoft[0])));
  dump_group("skip", kSkip, static_cast<int>(sizeof(kSkip) / sizeof(kSkip[0])));
  std::fprintf(out,
               "[hw-dk] +60f: flushes=%llu gpu_scanouts=%llu readback_MiB=%.2f upload_MiB=%.2f "
               "covers=%llu skipped=%llu tex_hit_flushes=%llu mem_flushes=%llu rtt_direct=%llu tex_drops=%llu waits=%llu victim_flushes=%llu coherence_flushes=%llu\n",
               d64(cur.flushes, prev.flushes), d64(cur.gpu_scanouts, prev.gpu_scanouts),
               (have_delta ? static_cast<double>(cur.readback_bytes - prev.readback_bytes)
                           : static_cast<double>(cur.readback_bytes)) /
                   1048576.0,
               (have_delta ? static_cast<double>(cur.upload_bytes - prev.upload_bytes)
                           : static_cast<double>(cur.upload_bytes)) /
                   1048576.0,
               d64(cur.seeds, prev.seeds), d64(cur.cpu_fallback, prev.cpu_fallback),
               d64(cur.tex_hit_flushes, prev.tex_hit_flushes), d64(cur.mem_access_flushes, prev.mem_access_flushes),
               d64(cur.rtt_direct, prev.rtt_direct), d64(cur.tex_targeted_drops, prev.tex_targeted_drops),
               d64(cur.waits, prev.waits), d64(cur.victim_flushes, prev.victim_flushes),
               d64(cur.coherence_flushes, prev.coherence_flushes));
  // NX-2026-11-27-AH: wall time of the window by part, in ms (the window is 60 guest frames).
  std::fprintf(out,
               "[hw-dk] time ms/60f: draw=%.1f wait=%.1f flush=%.1f rb_copy=%.1f cover=%.1f decode=%.1f texup=%.1f | "
               "decodes=%u texups=%u revalidated=%u idle_wait=%.1f idle_waits=%u | mem-access hook=%.1f hooks=%u "
               "tex hash=%.1f hashes=%u | hook_skips=%u upload copy=%.1f MiB=%.1f commits=%llu write_drops=%u superseded=%u\n",
               g_prof.draw / 1e6, g_prof.wait / 1e6, g_prof.flush / 1e6, g_prof.rb_copy / 1e6, g_prof.cover / 1e6,
               g_prof.decode / 1e6, g_prof.texup / 1e6, g_prof.decodes, g_prof.texups, g_prof.revalid, g_prof.idle_wait / 1e6, g_prof.idle_waits,
               g_prof.hook / 1e6, g_prof.hooks, g_prof.hash / 1e6, g_prof.hashes, g_prof.hook_skips,
               static_cast<double>(gt4_gs_upload_ns - g_upload_prev[0]) / 1e6,
               static_cast<double>(gt4_gs_upload_bytes - g_upload_prev[1]) / 1048576.0, gt4_gs_uploads - g_upload_prev[2],
               g_write_drops, g_superseded);
  g_write_drops = 0;
  g_superseded = 0;
  g_upload_prev[0] = gt4_gs_upload_ns;
  g_upload_prev[1] = gt4_gs_upload_bytes;
  g_upload_prev[2] = gt4_gs_uploads;
  g_prof = {};
  if (!g_hit_hist.empty()) {
    std::fprintf(out, "[hw-dk] tex-hit causes (tbp/psm>slot base/fbw/psm:count):");
    for (int shown = 0; shown < 5 && !g_hit_hist.empty(); ++shown) {
      auto best = g_hit_hist.begin();
      for (auto it = g_hit_hist.begin(); it != g_hit_hist.end(); ++it)
        if (it->second > best->second) best = it;
      const int slot = static_cast<int>(best->first >> 24);
      if (state_ && slot < kColourSlots) {
        const Surface &c = state_->colour[slot];
        std::fprintf(out, " %05x/%02x>%05x/%u/%02x:%u", best->first & 0xFFFFu, (best->first >> 16) & 0xFFu,
                     static_cast<unsigned>(c.view.basePointer), c.fbw, c.psm_code, best->second);
      }
      g_hit_hist.erase(best);
    }
    std::fprintf(out, "\n");
    g_hit_hist.clear();
  }
  hist_print(out, "cover causes (base/fbw/psm+rows[*=depth]:count(KiB))", g_cover_hist);
  hist_print(out, "mem-flush causes (base/fbw/psm+block[*=write]:count(KiB))", g_flush_hist);
  hist_print(out, "trim causes (base/fbw/psm+block[*=by write-back]:count(KiB lost))", g_trim_hist);
  std::fprintf(out, "[hw-dk] tex slots re-used /60f by class 4M/1M/512K/256K/64K/16K: %u %u %u %u %u %u | max ops in one command list=%u (limit %u)\n",
               g_tex_evict[0], g_tex_evict[1], g_tex_evict[2], g_tex_evict[3], g_tex_evict[4], g_tex_evict[5],
               state_ ? state_->max_list_ops : 0u, kMaxListOps);
  if (state_) state_->max_list_ops = 0;
  for (auto &e : g_tex_evict) e = 0;
  last_report_ = cur;
  has_last_report_ = true;
}

void WritebackDk::memory_written() {
#if defined(__SWITCH__)
  if (!state_) return;
  for (int i = 0; i < kColourSlots; ++i) state_->colour[i].seeded = {};
  for (auto &d : state_->depth_slots) d.seeded = {};
  invalidate_textures();
#endif
}

void WritebackDk::note_cpu_fallback() {
#if defined(__SWITCH__)
  // Counter only. Do NOT call memory_written(): that wiped seeded surfaces + the whole texture
  // cache on every refused batch (session log: 5746 fallbacks) and destroyed the GPU path.
  ++stats_.cpu_fallback;
#endif
}

void WritebackDk::invalidate_textures() {
#if defined(__SWITCH__)
  if (!state_) return;
  state_->tex_cache.clear();
  state_->tex_meta.clear();
  state_->tex_cache_bytes = 0;
  state_->slot_of.clear();
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
  // Fast path: do not WaitIdle on every GIF IMAGE/BITBLT. Drop GPU authority for the touched
  // region; only bust the texture cache when the write is outside the active colour/depth
  // targets (real TEX0 uploads). Car-select BITBLTs into the frame buffer used to wipe the
  // whole cache every packet and re-decode every sprite.
  if (!ready_ || !state_) return;
  State &s = *state_;
  ProfScope prof_hook(g_prof.hook);
  ++g_prof.hooks;
  // NX-2026-11-27-BD: a read only needs GPU pixels written back; with no dirty rows under the
  // range there is nothing to do. Run BC: about 1800 reads per frame in the race.
  if (!write) {
    bool any_dirty = false;
    for (int i = 0; i < kColourSlots && !any_dirty; ++i) {
      const Surface &c = s.colour[i];
      any_dirty = c.valid && !c.dirty.empty() && c.dirty_overlaps(block, blocks);
    }
    if (!any_dirty) {
      ++g_prof.hook_skips;
      return;
    }
  }
  if (s.seen_serial != s.draw_serial) {
    s.seen_serial = s.draw_serial;
    s.seen_n = s.seen_next = 0;
  }
  for (int i = 0; i < s.seen_n; ++i) {
    const State::SeenAccess &e = s.seen[i];
    if ((e.write || !write) && block >= e.block &&
        static_cast<std::uint64_t>(block) + blocks <= static_cast<std::uint64_t>(e.block) + e.blocks) {
      ++g_prof.hook_skips;
      return;
    }
  }
  s.seen[s.seen_next] = State::SeenAccess{block, blocks, write};
  s.seen_next = (s.seen_next + 1) % 16;
  if (s.seen_n < 16) ++s.seen_n;
  // NX-2026-11-27-S: GPU-drawn pixels that SLM has not seen yet must be written back before
  // anyone touches that memory. Dropping `dirty` without a flush lost the car-select frame
  // (re-seeded from stale SLM -> black). A read needs the pixels; a partial write keeps the
  // untouched part of the surface.
  // NX-2026-11-27-AA: write back only the slots whose dirty rows the access touches (was: all).
  unsigned dirty_mask = 0;
  for (int i = 0; i < kColourSlots; ++i) {
    if (!s.colour[i].dirty_overlaps(block, blocks)) continue;
    // NX-2026-11-27-BE: the write replaces all of it: nothing to write back.
    if (write && write_covers_dirty(s.colour[i])) {
      s.colour[i].dirty = {};
      ++g_write_drops;
      continue;
    }
    dirty_mask |= kSlotBit(i);
  }
  if (dirty_mask != 0) {
    for (int i = 0; i < kColourSlots; ++i) {
      if (!(dirty_mask & kSlotBit(i))) continue;
      const Surface &c = s.colour[i];
      hist_add(g_flush_hist, c.view.basePointer, c.fbw, c.psm_code, block, write,
               static_cast<std::uint64_t>(c.dirty.empty() ? 0 : (c.dirty.x1 - c.dirty.x0 + 1) * (c.dirty.y1 - c.dirty.y0 + 1)) * 4u);
      // NX-2026-11-27-BP: the access behind a write-back, one line per distinct (slot, block,
      // kind), at most 48 in a session: what was read or written (the upload rectangle when it
      // is a write) against the slot's unflushed rectangle and its block range.
      {
        static std::vector<std::uint64_t> seen_mf;
        const std::uint64_t id = static_cast<std::uint64_t>(c.view.basePointer) | (static_cast<std::uint64_t>(c.fbw) << 16) |
                                 (static_cast<std::uint64_t>(c.psm_code) << 24) | (static_cast<std::uint64_t>(block) << 32) |
                                 (static_cast<std::uint64_t>(write ? 1 : 0) << 48);
        if (seen_mf.size() < 48u && std::find(seen_mf.begin(), seen_mf.end(), id) == seen_mf.end()) {
          seen_mf.push_back(id);
          uint32_t db = 0, dn = 0;
          c.row_range(c.dirty.y0, c.dirty.y1, db, dn);
          const Gt4GsWriteInfo &w = gt4_gs_write_info;
          std::fprintf(stderr,
                       "[hw-dk] mem-flush: %s block=%05x blocks=%u | slot %05x/%u/%02x dirty (%d,%d)-(%d,%d) blocks %05x+%u "
                       "seeded (%d,%d)-(%d,%d) stamp=%llu | write info valid=%d base=%05x bw=%u psm=%02x rect %u,%u %ux%u "
                       "exact %05x+%u\n",
                       write ? "write" : "read", block, blocks, static_cast<unsigned>(c.view.basePointer), c.fbw, c.psm_code,
                       c.dirty.x0, c.dirty.y0, c.dirty.x1, c.dirty.y1, db, dn, c.seeded.x0, c.seeded.y0, c.seeded.x1, c.seeded.y1,
                       static_cast<unsigned long long>(c.dirty_stamp), (write && w.valid) ? 1 : 0, static_cast<unsigned>(w.base),
                       static_cast<unsigned>(w.bw), static_cast<unsigned>(w.psm), static_cast<unsigned>(w.x0),
                       static_cast<unsigned>(w.y0), static_cast<unsigned>(w.w), static_cast<unsigned>(w.h),
                       static_cast<unsigned>(w.exact_first), static_cast<unsigned>(w.exact_blocks));
        }
      }
    }
    std::string err;
    if (!flush_surfaces(s, stats_, local, dirty_mask, err)) {
      std::fprintf(stderr, "[hw-dk] mem-access flush failed: %s\n", err.c_str());
    }
    ++stats_.mem_access_flushes;
    invalidate_flushed(s, stats_); // NX-2026-11-27-T: only textures over written-back rows
  }
  bool hit_colour = false;
  for (int i = 0; i < kColourSlots; ++i) {
    if (s.colour[i].overlaps(block, blocks)) hit_colour = true;
    if (s.colour[i].dirty_overlaps(block, blocks)) s.colour[i].dirty = {};
  }
  bool hit_depth = false;
  for (auto &d : s.depth_slots) {
    if (d.overlaps(block, blocks)) hit_depth = true;
    if (d.dirty_overlaps(block, blocks)) d.dirty = {};
  }
  if (write) {
    // NX-2026-11-13-G: CommitUpload fields (dpsm/dbp/…) are not on this hook; rate-limit large
    // IMAGE/BITBLT into the colour target so owner stderr can grep FMV vs UI transfers.
    constexpr std::uint32_t kLargeBlocks = 32u; // 8 KiB (kBlockBytes=256)
    if (hit_colour && blocks >= kLargeBlocks) {
      static std::uint64_t image_diag_n = 0;
      if ((++image_diag_n % 30u) == 1u) {
        std::fprintf(stderr,
                     "[hw-dk] image-diag: write+hit_colour block=%u blocks=%u (~%u KiB)\n", block, blocks,
                     (blocks * memory::kBlockBytes) / 1024u);
        std::fflush(stderr);
      }
    }
    if (hit_colour) {
      // NX-2026-11-27-AA: keep the rows above the write. GS pages are linear (32 blocks each,
      // fbw pages per page row, >= 32 pixel rows per page), so every row below
      // page_row * 32 lies in memory before `block` and still equals SLM. Clearing the whole
      // rect made each texture upload inside a 1024-row sampled target force a 4 MiB reseed.
      // NX-2026-11-27-AO: same rule as a write-back of another slot (trim_seeded): keep the larger
      // part of `seeded` above or below the written page rows, and never cut `seeded` below
      // `dirty`. The AA rule above kept only rows above the write and could leave unflushed GPU
      // pixels outside `seeded`; the next draw then "seeded" those rows again from SLM over them.
      // In the race the game uploads into scratch targets that share memory with the 3D buffer,
      // so the 3D buffer got rows of old SLM, including 16-bit scratch pixels that appear in the
      // 32-bit view as 8-pixel columns every 64 pixels.
      for (int i = 0; i < kColourSlots; ++i) {
        Surface &c = s.colour[i];
        if (!c.overlaps(block, blocks)) continue;
        trim_seeded(c, block, blocks);
      }
    }
    if (hit_depth) {
      for (auto &d : s.depth_slots) {
        if (d.overlaps(block, blocks)) d.seeded = {};
      }
    }
    // NX-2026-11-27-AA: always drop decoded textures over the written blocks. Skipping this when
    // the write landed inside a colour box left stale decodes: the game uploads sprites and car
    // textures into memory that is also a (parked or sampled) render target.
    invalidate_textures_overlapping(s, block, blocks, stats_);
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
  bool colour_dirty = false;
  for (int i = 0; i < kColourSlots; ++i) {
    if (!s.colour[i].dirty.empty()) colour_dirty = true;
  }
  const bool had_dirty = colour_dirty || (!colour_only && !s.dz().dirty.empty());
  if (!flush_surfaces(s, stats_, local, colour_only ? kColourBit : (kColourBit | kDepthBit), error)) return false;
  // Pixels just written back may be sampled as a texture later.
  if (had_dirty) invalidate_flushed(s, stats_); // NX-2026-11-27-T: targeted
  return true;
#endif
}

bool WritebackDk::try_scanout(std::uint32_t fbp_pages, std::uint32_t fbw, std::uint32_t psm, std::int32_t dbx,
                              std::int32_t dby, std::uint32_t read_w, std::uint32_t read_h, std::uint32_t display_w,
                              std::uint32_t display_h, std::vector<std::uint32_t> &rgba,
                              memory::LocalMemory &local, std::string &error) {
  return scanout_impl(fbp_pages, fbw, psm, dbx, dby, read_w, read_h, display_w, display_h, &rgba, local, error);
}

// Zero-copy present: same surface match as try_scanout, then one GPU blit into the swapchain
// on the same queue. No readback, no WaitIdle, no CPU pixels.
bool WritebackDk::try_present_direct(std::uint32_t fbp_pages, std::uint32_t fbw, std::uint32_t psm, std::int32_t dbx,
                                     std::int32_t dby, std::uint32_t read_w, std::uint32_t read_h,
                                     std::uint32_t display_w, std::uint32_t display_h,
                                     memory::LocalMemory &local, std::string &error) {
  if (!gt4::platform::nx::deko3d_present_gpu_attached()) return false;
  return scanout_impl(fbp_pages, fbw, psm, dbx, dby, read_w, read_h, display_w, display_h, nullptr, local, error);
}

bool WritebackDk::scanout_impl(std::uint32_t fbp_pages, std::uint32_t fbw, std::uint32_t psm, std::int32_t dbx,
                               std::int32_t dby, std::uint32_t read_w, std::uint32_t read_h, std::uint32_t display_w,
                               std::uint32_t display_h, std::vector<std::uint32_t> *rgba,
                               memory::LocalMemory &local, std::string &error) {
#if !defined(__SWITCH__)
  (void)local;
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

  State &s = *state_;
  // NX-2026-11-13-I: stderr-only miss reasons (~1/60). NX-J: log both colour slot bases.
  const std::uint32_t want_base = memory::blockOfPage(fbp_pages);
  // NX-2026-11-27-AM: remember this display target (see State::display_base).
  if (!((s.display_base[0] == want_base && s.display_fbw[0] == fbw) ||
        (s.display_base[1] == want_base && s.display_fbw[1] == fbw))) {
    const unsigned k = s.display_n++ & 1u;
    s.display_base[k] = want_base;
    s.display_fbw[k] = fbw;
  }
  const std::uint64_t need_bytes = static_cast<std::uint64_t>(read_w) * read_h * 4u;
  // NX-2026-11-27-AB: lists every valid slot (was hard-coded to slots 0 and 1).
  auto log_miss = [&](const char *reason, const char *same_sub) {
    static std::uint64_t miss_n = 0;
    if ((++miss_n % 60u) != 1u) return;
    char slots[512];
    int at = 0;
    slots[0] = '\0';
    for (int i = 0; i < kColourSlots; ++i) {
      const Surface &c = s.colour[i];
      if (!c.valid) continue;
      const int n = std::snprintf(slots + at, sizeof(slots) - static_cast<std::size_t>(at), "%d:{base=%u fbw=%u psm=%u} ",
                                  i, static_cast<unsigned>(c.view.basePointer), c.fbw, c.psm_code);
      if (n < 0 || static_cast<std::size_t>(at + n) >= sizeof(slots)) break;
      at += n;
    }
    std::fprintf(stderr,
                 "[hw-dk] scanout-miss: reason=%s %s slots=[ %s] active=%d want={base=%u fbw=%u psm=%u} "
                 "rect=(%d,%d)+%ux%u disp=%ux%u bytes=%llu\n",
                 reason, same_sub != nullptr ? same_sub : "", slots, s.colour_active, want_base, fbw, psm, dbx, dby,
                 read_w, read_h, display_w, display_h, static_cast<unsigned long long>(need_bytes));
    std::fflush(stderr);
  };

  // CT16/CT16S are drawn into the RGBA8 colour target in accept_batch; allow GPU scanout when
  // same_target matches so menus/car select skip SyncForDisplay + CPU ComposeDisplay.
  if (psm != hwgs::psm::kCt32 && psm != hwgs::psm::kCt24 && psm != hwgs::psm::kCt16 &&
      psm != hwgs::psm::kCt16S) {
    log_miss("bad_psm", nullptr);
    return false;
  }
  // Present either matches the read rectangle or is a simple FFMD line-double of it.
  const bool line_double = display_w == read_w && display_h == read_h * 2u;
  if (!(display_w == read_w && display_h == read_h) && !line_double) {
    log_miss("size/line_double", nullptr);
    return false;
  }

  const auto decoded = memory::decodePsm(psm);
  if (!decoded) {
    log_miss("decode", nullptr);
    return false;
  }
  const memory::BufferView view{want_base, fbw, *decoded};
  // NX-2026-11-13-J: scan from any colour slot that same_target(DISPFB); predicate unchanged.
  int match_i = -1;
  std::int32_t scan_y_off = 0;
  bool uncovered = false;
  for (int i = 0; i < kColourSlots; ++i) {
    if (s.colour[i].same_target(view)) {
      match_i = i;
      break;
    }
  }
  // NX-2026-11-13-M: DISPFB at start of a page-row band inside a taller same-fbw/psm FRAME
  // (spec §5.10 step 2). Exact same_target misses when want_base = parent_base +
  // FootprintBlocks(fbw, y_off); still GPU-scanout from (dbx, dby+y_off). Draw-path
  // same_target() is untouched. L case: colour@0 + want@4480 → y_off=448.
  if (match_i < 0) {
    for (int i = 0; i < kColourSlots; ++i) {
      const Surface &slot = s.colour[i];
      if (!slot.valid || slot.fbw != fbw || slot.view.psm != view.psm) continue;
      if (want_base < slot.view.basePointer) continue;
      const std::uint32_t delta = want_base - slot.view.basePointer;
      for (std::int32_t y_off = 0; y_off <= kDkSurface; y_off += 32) {
        if (hwgs::FootprintBlocks(fbw, static_cast<std::uint32_t>(y_off)) != delta) continue;
        if (dbx < 0 || dby + y_off < 0) continue;
        if (dbx + static_cast<std::int32_t>(read_w) > kDkSurface) continue;
        if (dby + y_off + static_cast<std::int32_t>(read_h) > kDkSurface) continue;
        // NX-2026-11-27-AB: the band must already be on the GPU. Run 13 presented rows 448..671 of
        // the FRAME@0 slot, which nothing had drawn or seeded (black car select), while the real
        // DISPFB pixels sat in guest memory. Not covered: fall through to CPU compose.
        const Rect band{dbx, dby + y_off, dbx + static_cast<std::int32_t>(read_w) - 1,
                        dby + y_off + static_cast<std::int32_t>(read_h) - 1};
        if (!slot.seeded.contains(band)) {
          uncovered = true;
          continue;
        }
        match_i = i;
        scan_y_off = y_off;
        break;
      }
      if (match_i >= 0) break;
    }
  }
  if (match_i < 0) {
    char sub[280];
    const Surface &act = s.active_colour();
    bool any_valid = false;
    for (int i = 0; i < kColourSlots; ++i) any_valid = any_valid || s.colour[i].valid;
    if (uncovered) {
      std::snprintf(sub, sizeof(sub), "sub=band_uncovered");
    } else if (!any_valid) {
      std::snprintf(sub, sizeof(sub), "sub=!valid");
    } else {
      const bool base_miss = !act.valid || act.view.basePointer != view.basePointer;
      const bool fbw_miss = !act.valid || act.view.bufferWidth != view.bufferWidth;
      const bool psm_miss = !act.valid || act.view.psm != view.psm;
      const std::uint32_t act_base = act.valid ? act.view.basePointer : 0u;
      const std::uint32_t act_fbw = act.valid ? act.fbw : 0u;
      const std::uint32_t act_psm = act.valid ? act.psm_code : 0u;
      const int n = std::snprintf(
          sub, sizeof(sub),
          "sub=%s%s%s| base colour_base=%u want_base=%u | fbw colour=%u want=%u | psm colour=%u want=%u",
          base_miss ? "base " : "", fbw_miss ? "fbw " : "", psm_miss ? "psm " : "", act_base, want_base, act_fbw,
          fbw, act_psm, psm);
      if (n < 0 || static_cast<std::size_t>(n) >= sizeof(sub)) sub[sizeof(sub) - 1] = '\0';
    }
    log_miss("same_target", sub);
    return false;
  }
  Surface &scan_colour = s.colour[match_i];
  if (scan_y_off != 0) {
    static std::uint64_t half_n = 0;
    if ((++half_n % 60u) == 1u) {
      std::fprintf(stderr, "[hw-dk] scanout-half: y_off=%d base=%u want_base=%u slot=%d\n", scan_y_off,
                   scan_colour.view.basePointer, want_base, match_i);
      std::fflush(stderr);
    }
  }

  const Rect need{dbx, dby + scan_y_off, dbx + static_cast<int32_t>(read_w) - 1,
                  dby + scan_y_off + static_cast<int32_t>(read_h) - 1};
  if (need.x0 < 0 || need.y0 < 0 || need.x1 >= kDkSurface || need.y1 >= kDkSurface) {
    log_miss("rect/bytes", nullptr);
    return false;
  }

  const uint32_t rw = read_w, rh = read_h;
  if (need_bytes > kSurfaceBytes) {
    log_miss("rect/bytes", nullptr);
    return false;
  }

  // NX-2026-11-27-AB: make the slot hold what guest memory would show for `need`.
  // 1. Another slot with unflushed draws over this memory range (the fbw=2 alias targets draw into
  //    the DISPFB pages) is written back first; flush_surfaces then shrinks this slot's `seeded`
  //    to its own dirty area.
  // 2. Whatever part of `need` is not seeded is uploaded from guest memory. Until now a slot that
  //    had just been retargeted (seeded = its first draw only) was presented with stale rows.
  {
    const std::uint32_t blocks = scan_colour.rows_blocks(need.y1 + 1);
    unsigned mask = 0;
    for (int i = 0; i < kColourSlots; ++i) {
      if (i != match_i && s.colour[i].dirty_overlaps(scan_colour.view.basePointer, blocks)) mask |= kSlotBit(i);
    }
    if (mask != 0) {
      if (!flush_surfaces(s, stats_, local, mask, error)) return false;
      ++stats_.coherence_flushes;
      invalidate_flushed(s, stats_);
    }
    static std::uint64_t cover_n = 0;
    if (!scan_colour.seeded.contains(need) && (++cover_n % 60u) == 1u) {
      std::fprintf(stderr,
                   "[hw-dk] scanout-cover: slot=%d base=%u need=(%d,%d)-(%d,%d) seeded=(%d,%d)-(%d,%d) "
                   "dirty=(%d,%d)-(%d,%d) alias_mask=%x\n",
                   match_i, static_cast<unsigned>(scan_colour.view.basePointer), need.x0, need.y0, need.x1, need.y1,
                   scan_colour.seeded.x0, scan_colour.seeded.y0, scan_colour.seeded.x1, scan_colour.seeded.y1,
                   scan_colour.dirty.x0, scan_colour.dirty.y0, scan_colour.dirty.x1, scan_colour.dirty.y1, mask);
      std::fflush(stderr);
    }
    cover_surface(s, stats_, scan_colour, need, local);
    if (s.gpu_error) return false;
  }

  if (rgba == nullptr) {
    // Flush this frame's recorded draws (no wait); the present list queues behind them.
    s.submit(stats_);
    if (s.gpu_error) return false;
    if (!gt4::platform::nx::deko3d_present_gpu_image(&scan_colour.image, static_cast<std::uint32_t>(need.x0),
                                                     static_cast<std::uint32_t>(need.y0), rw, rh)) {
      log_miss("direct_present", nullptr);
      return false;
    }
    ++stats_.gpu_scanouts;
    ++stats_.direct_presents;
    (void)error;
    return true;
  }

  s.begin_recording();
  s.barrier();
  DkImageView colour_view;
  dkImageViewDefaults(&colour_view, &scan_colour.image);
  DkImageRect rect{static_cast<uint32_t>(need.x0), static_cast<uint32_t>(need.y0), 0, rw, rh, 1};
  DkCopyBuf dst{dkMemBlockGetGpuAddr(s.rb_mem), 0, 0};
  dkCmdBufCopyImageToBuffer(s.cmdbuf, &colour_view, &rect, &dst, 0);
  s.submit_and_wait(stats_);
  if (s.gpu_error) return false;
  dkMemBlockFlushCpuCache(s.rb_mem, 0, (rw * rh * 4u + 0xFFFu) & ~0xFFFu); // NX-2026-11-27-AH

  const auto *src = reinterpret_cast<const std::uint32_t *>(s.rb_cpu);
  rgba->resize(static_cast<std::size_t>(display_w) * display_h);
  if (line_double) {
    for (uint32_t y = 0; y < rh; ++y) {
      const std::uint32_t *row = src + static_cast<std::size_t>(y) * rw;
      std::memcpy(rgba->data() + static_cast<std::size_t>(y * 2u) * display_w, row, rw * 4u);
      std::memcpy(rgba->data() + static_cast<std::size_t>(y * 2u + 1u) * display_w, row, rw * 4u);
    }
  } else {
    std::memcpy(rgba->data(), src, static_cast<std::size_t>(rw) * rh * 4u);
  }
  stats_.readback_bytes += static_cast<std::uint64_t>(rw) * rh * 4u;
  ++stats_.gpu_scanouts;
  (void)error;
  return true;
#endif
}

// NX-2026-11-27-AE: diagnostics file gt4_dump.bin next to the NRO. Records: 4 x u32 (magic
// 'GDT4', kind, width, height), 3 x u64 (a, b, c), then width*height RGBA8 words.
// kind 1: decoded texture (a=TEX0, b=ALPHA, c=first vertex RGBA | TEST<<32 | ABE<<52).
// kind 2: car buffer 0x2700 read back from SLM after a flush (a=copy number).
static void dump_record(std::uint32_t kind, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint32_t w,
                        std::uint32_t h, const std::uint32_t *px) {
#if defined(__SWITCH__)
  // NX-2026-11-27-AX: diagnostics file switched off (strip hunt is over). Set to true to get
  // gt4_dump.bin again.
  static const bool kDumpOn = false;
  if (!kDumpOn) return;
  static bool started = false;
  const std::string path = (gt4::platform::executable_directory() / "gt4_dump.bin").string();
  FILE *f = std::fopen(path.c_str(), started ? "ab" : "wb");
  started = true;
  if (!f) return;
  const std::uint32_t head[4] = {0x34544447u, kind, w, h};
  const std::uint64_t meta[3] = {a, b, c};
  std::fwrite(head, sizeof(head), 1, f);
  std::fwrite(meta, sizeof(meta), 1, f);
  std::fwrite(px, 4u, static_cast<std::size_t>(w) * h, f);
  std::fclose(f);
#else
  (void)kind; (void)a; (void)b; (void)c; (void)w; (void)h; (void)px;
#endif
}

bool WritebackDk::draw_batch(const hwgs::Batch &batch, memory::LocalMemory &local, const clut::ClutUnit &clut,
                             std::string &error) {
  ProfScope prof_draw(g_prof.draw);
  if (state_) ++state_->draw_serial; // NX-2026-11-27-BC: see State::seen
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
  DepthUse depth = depth_use(ctx);
  if (depth.never) return true; // every pixel fails the depth test: nothing is written
  // Depth optional: draw in submission order when the depth path is off or the format is unknown.
  bool uses_depth = depth.any() && s.depth_available && z_format_known(z_psm_code(ctx.zbuf));
  if (depth.any() && !uses_depth) ++stats_.rejects[static_cast<int>(Reject::DepthUnavailable)];
  // NX-2026-11-27-AT: GT4 race colour correction bypass. Every frame the game runs each 64x32 page
  // of the finished 3D buffer through a 256-entry lookup table: three batches of 16 sprites read
  // the page as PSMT8 into a work target (pass 1), then eight sprites per page load that work
  // target as a palette to put the pixels back (pass 2). That is about 1540 draws, 294 write-backs
  // and as many decodes per frame for a per-channel table lookup (run AS: gs_gpu_draw_ms 90 of a
  // 188 ms frame). Both passes are recognised by shape and dropped, which leaves the 3D buffer as
  // rendered: the picture lacks the table (brightness / gamma curve) until it is applied in one
  // GPU pass. The table is logged once. An empty file `gamma_on` next to the NRO turns the
  // bypass off and runs the chain as the game sends it.
  {
    static int bypass = -1;
    if (bypass < 0) {
      const std::string flag = (gt4::platform::executable_directory() / "gamma_on").string();
      FILE *f = std::fopen(flag.c_str(), "rb");
      if (f) std::fclose(f);
      bypass = f ? 0 : 1;
      std::fprintf(stderr, "[hw-dk] colour-correction bypass: %s\n", bypass ? "ON (create file gamma_on to disable)" : "off");
    }
    const uint32_t cc_tp = hwgs::tex0::Psm(ctx.tex0);
    if (bypass == 1 && tme && cc_tp == 0x13u && (prim & 7u) == 6u && bit(prim, hwgs::primbit::kFst)) {
      const uint32_t cc_tw = 1u << hwgs::tex0::Tw(ctx.tex0), cc_th = 1u << hwgs::tex0::Th(ctx.tex0);
      const uint32_t cc_tbp = static_cast<uint32_t>(hwgs::tex0::Tbp0(ctx.tex0));
      const uint32_t cc_cbp = static_cast<uint32_t>((ctx.tex0 >> 37) & 0x3FFFu);
      const uint32_t cc_fb = static_cast<uint32_t>(hwgs::frame::Fbp(ctx.frame)) * 32u;
      const uint32_t cc_fpsm = static_cast<uint32_t>(hwgs::frame::Psm(ctx.frame));
      const uint32_t cc_msk = static_cast<uint32_t>(hwgs::frame::Fbmsk(ctx.frame));
      const bool cc_pass1 = cc_tw == 128u && cc_th == 64u && hwgs::tex0::Tbw(ctx.tex0) == 2u && batch.vertices.size() == 32u &&
                            cc_fpsm == hwgs::psm::kCt32 && cc_msk == 0xFF000000u && cc_cbp == cc_fb + 0x20u;
      const bool cc_pass2 = cc_tw == 8u && cc_th == 32u && batch.vertices.size() == 2u && cc_fpsm == hwgs::psm::kCt24 &&
                            cc_msk == 0xFF000000u && hwgs::tex0::Tfx(ctx.tex0) == 1u && cc_cbp + 0x24u >= cc_tbp &&
                            cc_cbp < cc_tbp && ((cc_tbp - cc_cbp) & 3u) == 0u;
      if (cc_pass1 || cc_pass2) {
        static std::uint64_t dropped = 0;
        static bool table_logged = false;
        if (cc_pass1 && !table_logged) {
          table_logged = true;
          const auto pal = clut.Decode(cc_tp, hwgs::tex0::Cpsm(ctx.tex0), hwgs::tex0::Csa(ctx.tex0), batch.state.env.texa);
          std::fprintf(stderr, "[hw-dk] colour-correction table (index:r,g,b every 16th, count=%u):", pal.count);
          for (uint32_t i = 0; i < pal.count; i += 16u) {
            const uint32_t e = pal.entries[i];
            std::fprintf(stderr, " %u:%u,%u,%u", i, e & 0xFFu, (e >> 8) & 0xFFu, (e >> 16) & 0xFFu);
          }
          if (pal.count == 256u) {
            const uint32_t e = pal.entries[255];
            std::fprintf(stderr, " 255:%u,%u,%u", e & 0xFFu, (e >> 8) & 0xFFu, (e >> 16) & 0xFFu);
            int max_dev = 0;
            for (uint32_t i = 0; i < 256u; ++i) {
              const uint32_t x = pal.entries[i];
              for (int ch = 0; ch < 3; ++ch) {
                const int d = std::abs(static_cast<int>((x >> (8 * ch)) & 0xFFu) - static_cast<int>(i));
                if (d > max_dev) max_dev = d;
              }
            }
            std::fprintf(stderr, " | max deviation from identity=%d", max_dev);
          }
          std::fprintf(stderr, "\n");
        }
        if ((++dropped % 100000u) == 1u)
          std::fprintf(stderr, "[hw-dk] colour-correction bypass: %llu batches dropped\n",
                       static_cast<unsigned long long>(dropped));
        return true;
      }
    }
  }
  // NX-2026-11-27-AQ: race post-process trace. The game reads the 3D buffer (0x1a40) page by page
  // as PSMT8 into a work target at 0x3fa0 and draws back into the 24-bit view of 0x1a40. First 60
  // batches of those two passes with every vertex, plus state the W16 line does not carry.
  {
    static uint32_t pp = 0;
    const uint32_t fb_base = hwgs::frame::Fbp(ctx.frame) * 32u;
    const uint32_t pp_tp = hwgs::tex0::Psm(ctx.tex0), pp_tw = 1u << hwgs::tex0::Tw(ctx.tex0), pp_th = 1u << hwgs::tex0::Th(ctx.tex0);
    // NX-2026-11-27-AR: recognised by texture shape, wherever the work target lies (it moves: 0x3fa0, 0x3060).
    const bool pass1 = pp_tp == 0x13u && pp_tw == 128u && pp_th == 64u && fb_base != 0u;
    const bool pass2 = pp_tp == 0x13u && pp_tw == 8u && pp_th == 32u;
    static uint32_t pp_seen = 0;
    if (tme && (pass1 || pass2)) ++pp_seen;
    if (tme && (pass1 || pass2) && (pp < 40 || (pp_seen > 20000 && pp < 70))) {
      ++pp;
      std::fprintf(stderr,
                   "[hw-dk] PP #%u pass%d: prim=%u nv=%u abe=%d fst=%d | frame fbp=%05x fbw=%u psm=%02x fbmsk=%08x | tex0=%016llx "
                   "tbp=%05x tbw=%u psm=%02x tw=%u th=%u tcc=%u tfx=%u cbp=%05x cpsm=%u csm=%u csa=%u | tex1=%010llx "
                   "clamp=%011llx texa=%010llx alpha=%010llx test=%05x scissor=%012llx zbuf=%010llx |",
                   pp, pass1 ? 1 : 2, static_cast<unsigned>(prim & 7), static_cast<unsigned>(batch.vertices.size()),
                   abe ? 1 : 0, bit(prim, hwgs::primbit::kFst) ? 1 : 0, static_cast<unsigned>(hwgs::frame::Fbp(ctx.frame)),
                   static_cast<unsigned>(hwgs::frame::Fbw(ctx.frame)), static_cast<unsigned>(hwgs::frame::Psm(ctx.frame)),
                   static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)), static_cast<unsigned long long>(ctx.tex0),
                   static_cast<unsigned>(hwgs::tex0::Tbp0(ctx.tex0)), static_cast<unsigned>(hwgs::tex0::Tbw(ctx.tex0)),
                   static_cast<unsigned>(hwgs::tex0::Psm(ctx.tex0)), 1u << hwgs::tex0::Tw(ctx.tex0),
                   1u << hwgs::tex0::Th(ctx.tex0), static_cast<unsigned>(hwgs::tex0::Tcc(ctx.tex0)),
                   static_cast<unsigned>(hwgs::tex0::Tfx(ctx.tex0)), static_cast<unsigned>((ctx.tex0 >> 37) & 0x3FFFu),
                   static_cast<unsigned>(hwgs::tex0::Cpsm(ctx.tex0)), static_cast<unsigned>((ctx.tex0 >> 55) & 1u),
                   static_cast<unsigned>(hwgs::tex0::Csa(ctx.tex0)), static_cast<unsigned long long>(ctx.tex1 & 0xffffffffffull),
                   static_cast<unsigned long long>(ctx.clamp & 0xfffffffffffull),
                   static_cast<unsigned long long>(batch.state.env.texa & 0xffffffffffull),
                   static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), static_cast<unsigned>(ctx.test & 0xFFFFFu),
                   static_cast<unsigned long long>(ctx.scissor & 0xffffffffffffull),
                   static_cast<unsigned long long>(ctx.zbuf & 0xffffffffffull));
      for (std::size_t i = 0; i < batch.vertices.size() && i < 34; ++i) {
        const auto &v = batch.vertices[i];
        std::fprintf(stderr, " [%.2f,%.2f %.2f,%.2f %02x%02x%02x%02x]", v.x4 / 16.0, v.y4 / 16.0, v.u / 16.0, v.v / 16.0, v.r,
                     v.g, v.b, v.a);
      }
      std::fprintf(stderr, "\n");
    }
  }
  // NX-2026-11-27-BQ: draws into narrow targets (FBW 1 or 2). Run BP: the write-backs left in
  // the race come from 64-wide 32-bit "targets" inside the texture area that the game later
  // uploads over or samples as textures. One line per distinct (frame, texture, primitive, vertex
  // count), at most 60 in a session: full state and the first six vertices.
  {
    const uint32_t nfbw = static_cast<uint32_t>(hwgs::frame::Fbw(ctx.frame));
    if (nfbw == 1u) { // NX-2026-11-27-BR: was <= 2; see the note at the key below
      static std::vector<std::uint64_t> seen_narrow;
      // NX-2026-11-27-BR: keyed by frame, primitive and textured only. Run BQ spent all 60 lines
      // on FBW 2 menu draws that differed in vertex count, before the race began.
      const std::uint64_t id = static_cast<std::uint64_t>(hwgs::frame::Fbp(ctx.frame)) |
                               (static_cast<std::uint64_t>(prim & 7u) << 32) | (static_cast<std::uint64_t>(tme ? 1 : 0) << 36) |
                               (static_cast<std::uint64_t>(hwgs::frame::Psm(ctx.frame)) << 40);
      if (seen_narrow.size() < 60u && std::find(seen_narrow.begin(), seen_narrow.end(), id) == seen_narrow.end()) {
        seen_narrow.push_back(id);
        std::fprintf(stderr,
                     "[hw-dk] narrow: prim=%u nv=%u tme=%d abe=%d fst=%d | frame=%016llx fbp=%05x fbw=%u psm=%02x fbmsk=%08x | "
                     "tex0=%016llx tbp=%05x tbw=%u psm=%02x %ux%u tcc=%u tfx=%u | tex1=%010llx clamp=%011llx texa=%010llx "
                     "alpha=%010llx test=%05x scissor=%012llx zbuf=%010llx |",
                     static_cast<unsigned>(prim & 7u), static_cast<unsigned>(batch.vertices.size()), tme ? 1 : 0, abe ? 1 : 0,
                     bit(prim, hwgs::primbit::kFst) ? 1 : 0, static_cast<unsigned long long>(ctx.frame),
                     static_cast<unsigned>(hwgs::frame::Fbp(ctx.frame)), nfbw, static_cast<unsigned>(hwgs::frame::Psm(ctx.frame)),
                     static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)), static_cast<unsigned long long>(ctx.tex0),
                     static_cast<unsigned>(hwgs::tex0::Tbp0(ctx.tex0)), static_cast<unsigned>(hwgs::tex0::Tbw(ctx.tex0)),
                     static_cast<unsigned>(hwgs::tex0::Psm(ctx.tex0)), 1u << hwgs::tex0::Tw(ctx.tex0),
                     1u << hwgs::tex0::Th(ctx.tex0), static_cast<unsigned>(hwgs::tex0::Tcc(ctx.tex0)),
                     static_cast<unsigned>(hwgs::tex0::Tfx(ctx.tex0)), static_cast<unsigned long long>(ctx.tex1 & 0xffffffffffull),
                     static_cast<unsigned long long>(ctx.clamp & 0xfffffffffffull),
                     static_cast<unsigned long long>(batch.state.env.texa & 0xffffffffffull),
                     static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), static_cast<unsigned>(ctx.test & 0xFFFFFu),
                     static_cast<unsigned long long>(ctx.scissor & 0xffffffffffffull),
                     static_cast<unsigned long long>(ctx.zbuf & 0xffffffffffull));
        for (std::size_t i = 0; i < batch.vertices.size() && i < 6; ++i) {
          const auto &v = batch.vertices[i];
          std::fprintf(stderr, " [xy %.2f,%.2f uv %.2f,%.2f stq %.4f,%.4f,%.4f %02x%02x%02x%02x]", v.x4 / 16.0, v.y4 / 16.0,
                       v.u / 16.0, v.v / 16.0, v.s, v.t, v.q, v.r, v.g, v.b, v.a);
        }
        std::fprintf(stderr, "\n");
      }
    }
  }
  // NX-2026-11-27-AN: race strip trace. Sprites that sample a direct-colour texture while the
  // frame is 16-bit (the screen copies), first 400: state plus the first two vertices.
  {
    static uint32_t w16 = 0;
    const uint32_t tp = hwgs::tex0::Psm(ctx.tex0);
    if (w16 < 0 * 400 && tme && (prim & 7u) == 6u && hwgs::frame::Psm(ctx.frame) == hwgs::psm::kCt16 &&
        (tp == 0u || tp == 1u || tp == 2u || tp == 0x0Au) && batch.vertices.size() >= 2) {
      ++w16;
      const auto &a = batch.vertices[0];
      const auto &b = batch.vertices[1];
      std::fprintf(stderr,
                   "[hw-dk] W16 #%u: nv=%u abe=%d fst=%d | frame fbp=%05x fbw=%u fbmsk=%08x | tex tbp=%05x tbw=%u psm=%02x "
                   "tw=%u th=%u tcc=%u tfx=%u | clamp=%011llx alpha=%010llx test=%05x | v0 xy=%.2f,%.2f uv=%.2f,%.2f "
                   "stq=%.5f,%.5f,%.5f | v1 xy=%.2f,%.2f uv=%.2f,%.2f stq=%.5f,%.5f,%.5f\n",
                   w16, static_cast<unsigned>(batch.vertices.size()), abe ? 1 : 0, bit(prim, hwgs::primbit::kFst) ? 1 : 0,
                   static_cast<unsigned>(hwgs::frame::Fbp(ctx.frame)), static_cast<unsigned>(hwgs::frame::Fbw(ctx.frame)),
                   static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)), static_cast<unsigned>(hwgs::tex0::Tbp0(ctx.tex0)),
                   static_cast<unsigned>(hwgs::tex0::Tbw(ctx.tex0)), tp, 1u << hwgs::tex0::Tw(ctx.tex0),
                   1u << hwgs::tex0::Th(ctx.tex0), static_cast<unsigned>(hwgs::tex0::Tcc(ctx.tex0)),
                   static_cast<unsigned>(hwgs::tex0::Tfx(ctx.tex0)), static_cast<unsigned long long>(ctx.clamp & 0xfffffffffffull),
                   static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), static_cast<unsigned>(ctx.test & 0xFFFFFu),
                   a.x4 / 16.0, a.y4 / 16.0, a.u / 16.0, a.v / 16.0, a.s, a.t, a.q, b.x4 / 16.0, b.y4 / 16.0, b.u / 16.0,
                   b.v / 16.0, b.s, b.t, b.q);
    }
  }
  // NX-2026-11-27-W: car-select black-screen trace. Once the 3D phase starts (enough depth-using
  // batches seen), dump the full GS draw state of every batch, capped.
  {
    static uint32_t depth_seen = 0, traced = 0;
    if (depth.any()) ++depth_seen;
    if (depth_seen >= 280 && traced < 700) {
      ++traced;
      std::fprintf(stderr,
                   "[hw-dk] W-draw #%u: prim=%u tme=%d abe=%d fst=%d nv=%u | frame fbp=%05x fbw=%u psm=%02x fbmsk=%08x | "
                   "zbuf zbp=%05x zpsm=%02x zmsk=%u | test ate=%u atst=%u aref=%u afail=%u date=%u datm=%u zte=%u ztst=%u | "
                   "alpha=%010llx | tex0 tbp=%05x tbw=%u psm=%02x tw=%u th=%u tcc=%u tfx=%u | area %d,%d-%d,%d\n",
                   traced, static_cast<unsigned>(prim & 7), tme ? 1 : 0, abe ? 1 : 0, bit(prim, hwgs::primbit::kFst) ? 1 : 0,
                   static_cast<unsigned>(batch.vertices.size()), static_cast<unsigned>(hwgs::frame::Fbp(ctx.frame)),
                   static_cast<unsigned>(hwgs::frame::Fbw(ctx.frame)), static_cast<unsigned>(hwgs::frame::Psm(ctx.frame)),
                   static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)), static_cast<unsigned>(BitRange(ctx.zbuf, 0, 8)),
                   static_cast<unsigned>(BitRange(ctx.zbuf, 24, 27)), static_cast<unsigned>(hwgs::zbuf::Zmsk(ctx.zbuf)),
                   static_cast<unsigned>(hwgs::test::Ate(ctx.test)), static_cast<unsigned>(hwgs::test::Atst(ctx.test)),
                   static_cast<unsigned>(hwgs::test::Aref(ctx.test)), static_cast<unsigned>(hwgs::test::Afail(ctx.test)),
                   static_cast<unsigned>(hwgs::test::Date(ctx.test)), static_cast<unsigned>(BitRange(ctx.test, 15, 15)),
                   static_cast<unsigned>(hwgs::test::Zte(ctx.test)), static_cast<unsigned>(hwgs::test::Ztst(ctx.test)),
                   static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), static_cast<unsigned>(BitRange(ctx.tex0, 0, 13)),
                   static_cast<unsigned>(BitRange(ctx.tex0, 14, 19)), static_cast<unsigned>(BitRange(ctx.tex0, 20, 25)),
                   1u << hwgs::tex0::Tw(ctx.tex0), 1u << hwgs::tex0::Th(ctx.tex0), static_cast<unsigned>(hwgs::tex0::Tcc(ctx.tex0)),
                   static_cast<unsigned>(hwgs::tex0::Tfx(ctx.tex0)), batch.drawRect.x0, batch.drawRect.y0, batch.drawRect.x1, batch.drawRect.y1);
    }
  }

  // Pixel area: scissor (inclusive) intersected with the batch coverage, clamped to the surface.
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
  area = intersect(area, Rect{0, 0, kDkSurface - 1, kDkSurface - 1});
  if (area.empty()) return true;

  // Targets. A different frame or Z buffer: write the old one back and start a fresh GPU copy.
  const uint32_t frame_psm_code = hwgs::frame::Psm(ctx.frame);
  const uint32_t fbw = hwgs::frame::Fbw(ctx.frame);
  const auto frame_psm = memory::decodePsm(frame_psm_code);
  if (!frame_psm) return reject(stats_, Reject::FramePsm);
  const memory::BufferView frame_view{memory::blockOfPage(hwgs::frame::Fbp(ctx.frame)), fbw, *frame_psm};
  // NX-2026-11-13-J: dual colour slots. Park the abandoned FRAME-tagged DkImage so try_scanout
  // can still same_target(DISPFB). Keep D flush-before-retarget on the abandoned slot only;
  // do not overwrite the other slot's base metadata/image when activating a free/LRU victim.
  {
    Surface &cur = s.active_colour();
    if (!cur.same_target(frame_view)) {
      // NX-2026-11-27-AA: any slot already holding this target wins; else a free slot; else the
      // least recently used slot other than the active one.
      int other_i = -1;
      for (int i = 0; i < kColourSlots; ++i) {
        if (i != s.colour_active && s.colour[i].same_target(frame_view)) other_i = i;
      }
      if (other_i >= 0) {
        s.colour_active = other_i;
      } else {
        for (int i = 0; i < kColourSlots && other_i < 0; ++i) {
          if (i != s.colour_active && !s.colour[i].valid) other_i = i;
        }
        if (other_i < 0) {
          // NX-2026-11-27-AM: least recently used among slots that are not display targets.
          for (int i = 0; i < kColourSlots; ++i) {
            if (i == s.colour_active || s.is_display_target(s.colour[i])) continue;
            if (other_i < 0 || s.colour[i].stamp < s.colour[other_i].stamp) other_i = i;
          }
        }
        if (other_i < 0) {
          for (int i = 0; i < kColourSlots; ++i) {
            if (i == s.colour_active) continue;
            if (other_i < 0 || s.colour[i].stamp < s.colour[other_i].stamp) other_i = i;
          }
        }
        // NX-2026-11-27-Z: park current WITHOUT flushing. Its GPU image is not overwritten here
        // (the victim is the other slot), so dirty pixels stay valid on the GPU and keep `dirty`.
        // SLM gets them later only when needed: eviction below on a future retarget,
        // before_memory_access overlap, texture-hit flush, or flush_to_slm. Flushing on every
        // retarget cost one CopyImageToBuffer + WaitIdle per switch (run 9: 421 flushes, 493
        // waits per 60 frames) and was the car-select frame-rate collapse.
        // Keep seeded (NX-2026-11-27-R) plus valid/view/image for DISPFB scanout.
        // Bind new FRAME on free (!valid) other, else LRU other (overwrite only that slot).
        Surface &victim = s.colour[other_i];
        if (victim.valid && !victim.same_target(frame_view) && !victim.dirty.empty()) {
          if (!flush_surfaces(s, stats_, local, kSlotBit(other_i), error)) return false;
          ++stats_.victim_flushes;
          invalidate_flushed(s, stats_); // NX-2026-11-27-T: targeted
        }
        victim.dirty = {};
        victim.seeded = {};
        victim.view = frame_view;
        victim.fbw = fbw;
        victim.psm_code = frame_psm_code;
        victim.valid = true;
        s.colour_active = other_i;
      }
    }
    s.active_colour().stamp = ++s.use_clock;
  }

  if (uses_depth) {
    const uint32_t zcode = z_psm_code(ctx.zbuf);
    const auto z_psm = memory::decodePsm(zcode);
    if (!z_psm) {
      uses_depth = false;
    } else {
      const memory::BufferView z_view{memory::blockOfPage(hwgs::zbuf::Zbp(ctx.zbuf)), fbw, *z_psm};
      // NX-2026-11-27-AJ: the slot already holding this Z target, else a free one, else the
      // least recently used one (its depth is dropped; depth is never written back).
      int zi = -1;
      for (int i = 0; i < kDepthSlots; ++i) {
        if (s.depth_slots[i].same_target(z_view)) zi = i;
      }
      if (zi < 0) {
        for (int i = 0; i < kDepthSlots && zi < 0; ++i) {
          if (!s.depth_slots[i].valid) zi = i;
        }
        if (zi < 0) {
          zi = 0;
          for (int i = 1; i < kDepthSlots; ++i) {
            if (s.depth_slots[i].stamp < s.depth_slots[zi].stamp) zi = i;
          }
          // NX-2026-11-27-BV: a slot that holds no depth (nothing seeded: cleared by a write to its
          // memory, or never drawn) loses nothing; take the oldest of those before any slot that
          // still holds depth.
          {
            int empty = -1;
            for (int i = 0; i < kDepthSlots; ++i) {
              if (!s.depth_slots[i].seeded.empty()) continue;
              if (empty < 0 || s.depth_slots[i].stamp < s.depth_slots[empty].stamp) empty = i;
            }
            if (empty >= 0) zi = empty;
          }
          // NX-2026-11-27-BT: a depth target lost its slot (its depth is gone). First 24 only.
          static unsigned evict_n = 0;
          if (evict_n < 24u) {
            ++evict_n;
            const Surface &old = s.depth_slots[zi];
            std::fprintf(stderr, "[hw-dk] depth evict #%u: %05x/%u/%02x seeded (%d,%d)-(%d,%d) for %05x/%u/%02x\n", evict_n,
                         static_cast<unsigned>(old.view.basePointer), old.fbw, old.psm_code, old.seeded.x0, old.seeded.y0,
                         old.seeded.x1, old.seeded.y1, static_cast<unsigned>(z_view.basePointer), fbw, zcode);
          }
        }
        s.depth_slots[zi].dirty = {};
        s.depth_slots[zi].seeded = {};
      }
      s.depth_active = zi;
      s.dz().stamp = s.use_clock;
      s.dz().view = z_view;
      s.dz().fbw = fbw;
      s.dz().psm_code = zcode;
      s.dz().valid = true;
    }
  }

  // Texture sampling from a dirty GPU frame: prefer speed — invalidate the tex cache after a
  // deferred colour flush only when the overlap is large; otherwise draw with a stale decode.
  int direct_rtt = -1; // NX-2026-11-27-T: colour slot sampled directly, or -1
  // NX-2026-11-27-AE: the directly sampled slot is CT24. Its GPU alpha is not the texel alpha
  // (TEXA.TA0), so the shader runs with TCC=0; see the condition where direct_rtt is chosen.
  bool direct_ct24 = false;
  bool self_snap = false; // NX-2026-11-27-AU: sample a GPU copy of the active target (see State::snap)
  if (tme) {
    const uint64_t tex0 = ctx.tex0;
    // NX-2026-11-27-AD: footprint by texture format (see tex_footprint_blocks).
    // NX-2026-11-27-AE: width is TBW as stored. AD widened it to cover TW, so the 512-texel view
    // of a 320-pixel blur buffer (tbw=5) reached the next blur buffer, counted as a hit on the
    // active target and fell from direct sampling back to a readback on every blur pass.
    // NX-2026-11-27-AN: with REGION_CLAMP on T no texel below MAXV can be read, so the footprint
    // stops there. The race HUD uses a 128x1024 PSMT4 texture at block 0x3f42 whose declared
    // height runs past the end of GS memory and wraps onto the display buffer at block 0; every
    // HUD draw counted as a hit on the display target and wrote it back (296 per frame).
    uint32_t tex_rows = 1u << std::min<uint32_t>(hwgs::tex0::Th(tex0), 10u);
    if (hwgs::clampreg::Wmt(ctx.clamp) == 2u)
      tex_rows = std::min<uint32_t>(tex_rows, static_cast<uint32_t>(hwgs::clampreg::Maxv(ctx.clamp)) + 1u);
    // NX-2026-11-27-AO: and by the largest V any vertex of the batch has (plus one row for
    // filtering), when every V lies inside the texture. MAXV alone did not help: the HUD texture
    // keeps MAXV at its full declared height.
    {
      const bool fst_uv = bit(prim, hwgs::primbit::kFst);
      const double th_d = static_cast<double>(1u << std::min<uint32_t>(hwgs::tex0::Th(tex0), 10u));
      double v_lo = 1e30, v_hi = -1e30;
      for (const auto &vx : batch.vertices) {
        const double v = fst_uv ? vx.v / 16.0 : (vx.q != 0.0f ? static_cast<double>(vx.t) / vx.q * th_d : 1e30);
        v_lo = std::min(v_lo, v);
        v_hi = std::max(v_hi, v);
      }
      if (v_lo >= 0.0 && v_hi < th_d) tex_rows = std::min<uint32_t>(tex_rows, static_cast<uint32_t>(v_hi) + 2u);
    }
    const uint32_t tex_blocks = tex_footprint_blocks(hwgs::tex0::Psm(tex0), hwgs::tex0::Tbw(tex0), tex_rows);
    // NX-2026-11-27-AA: remember which slots hold unflushed pixels under the texture, so the
    // flush path writes back only those.
    unsigned tex_hit_mask = 0;
    for (int i = 0; i < kColourSlots; ++i) {
      if (s.colour[i].dirty_overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks)) tex_hit_mask |= kSlotBit(i);
    }
    const bool colour_tex_hit = tex_hit_mask != 0;
    // NX-2026-11-27-T: a CT32 texture at the exact base/width of the other colour surface is
    // sampled straight from its GPU image (no readback, no decode, no wait).
    const uint32_t ttw = 1u << hwgs::tex0::Tw(tex0), tth = 1u << hwgs::tex0::Th(tex0);
    const uint32_t twms = hwgs::clampreg::Wms(ctx.clamp), twmt = hwgs::clampreg::Wmt(ctx.clamp);
    const char *path = "decode";
    if (colour_tex_hit) {
      const bool active_hit = s.active_colour().dirty_overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks);
      // NX-2026-11-27-X: REPEAT (wm 0) is accepted too; it is sampled as REGION_REPEAT with mask
      // tw-1 / th-1 so the wrap stays inside the texture rectangle of the 1024x1024 image.
      // NX-2026-11-27-AA: search every parked slot, not just "the other one".
      // NX-2026-11-27-AE: a CT24 texture over a CT24 slot is sampled directly too (the car
      // select copies the 24-bit display buffer every frame; each copy was a 640x224 readback
      // plus a 1 MiB decode and upload). A CT24 texel has alpha TEXA.TA0. With TCC=0 the texel
      // alpha is unused; with TCC=1, MODULATE, TA0=0x80 and AEM=0 the result Af*0x80>>7 equals
      // Af, which is what the shader computes for TCC=0. Other cases keep the readback path.
      const uint32_t tpsm_code = hwgs::tex0::Psm(tex0);
      const uint64_t dtexa = batch.state.env.texa;
      const bool ct24_ok = tpsm_code == hwgs::psm::kCt24 &&
                           (hwgs::tex0::Tcc(tex0) == 0u ||
                            (hwgs::tex0::Tfx(tex0) == 0u && (dtexa & 0xFFu) == 0x80u && ((dtexa >> 15) & 1u) == 0u));
      // NX-2026-11-27-AM: CT16 / CT16S textures over a slot of the same format, same rule as CT24
      // with both TEXA values (a CT16 texel has alpha TA0 or TA1 by its A bit). The race renders
      // to 16-bit targets and samples them; without this every such draw was a write-back, a
      // decode and an upload (run AL: 298 write-backs and 1535 decodes per frame). The GPU copy
      // keeps 8 bits per channel where a decode would see 5; the difference is below one step.
      const bool ct16_ok = (tpsm_code == hwgs::psm::kCt16 || tpsm_code == hwgs::psm::kCt16S) &&
                           (hwgs::tex0::Tcc(tex0) == 0u ||
                            (hwgs::tex0::Tfx(tex0) == 0u && (dtexa & 0xFFu) == 0x80u && ((dtexa >> 32) & 0xFFu) == 0x80u &&
                             ((dtexa >> 15) & 1u) == 0u));
      // NX-2026-11-27-AU: CT16 with TCC=1, TA0=0, TA1=0x80, AEM=0 is sampled with the GPU alpha as
      // it is (the texel alpha would be 0 or 0x80 by the A bit; the GPU copy holds the alpha that
      // was drawn). The race samples its 16-bit display buffer this way every frame; the strict
      // rule sent a 1024x512 decode and upload through the slow path each time.
      const bool ct16_raw = (tpsm_code == hwgs::psm::kCt16 || tpsm_code == hwgs::psm::kCt16S) &&
                            hwgs::tex0::Tcc(tex0) == 1u && (dtexa & 0xFFu) == 0u && ((dtexa >> 32) & 0xFFu) == 0x80u &&
                            ((dtexa >> 15) & 1u) == 0u;
      // NX-2026-11-27-AX: texel alpha cannot reach the frame when the draw has no blending, no
      // alpha test, no destination alpha test, and the frame alpha is write-protected (FBMSK top
      // byte set, or a 24-bit frame). Then a CT24 / CT16 target is sampled directly whatever
      // TEXA says. Car select copies its 24-bit display buffer this way with TA0=0, which the
      // TEXA rule refused: 4 write-backs per frame (`00000/01>00000/10/01`).
      // NX-2026-11-27-BE: a CT24 texel's alpha is TEXA.TA0 for every texel when AEM = 0, whatever
      // TA0 and the texture function are; the vertex alpha carries it (VertexSetup::alpha_op).
      // Car select draws its 24-bit display buffers into a 32-bit target with alpha written
      // (run BD: `00000/01>00000/10/01`, 4 write-backs per frame).
      const bool ct24_const = tpsm_code == hwgs::psm::kCt24 && ((dtexa >> 15) & 1u) == 0u;
      const bool alpha_dead = tpsm_code != hwgs::psm::kCt32 && !abe && !hwgs::test::Ate(ctx.test) &&
                              !hwgs::test::Date(ctx.test) &&
                              (frame_psm_code == hwgs::psm::kCt24 || (hwgs::frame::Fbmsk(ctx.frame) >> 24) == 0xFFu);
      // NX-2026-11-27-BM: a CT32 texture over a CT24 slot (or the reverse) holds the same colour
      // bytes; only the alpha differs. When alpha cannot reach the frame (no blending, no alpha
      // or destination alpha test, frame alpha write-protected) the slot is sampled directly as
      // well. Run BL, car select: `01a40/00>...` one write-back, a 1024x512 decode and a 2 MiB
      // upload per frame (about 30 ms of the GS thread).
      const bool rgb_only = !abe && !hwgs::test::Ate(ctx.test) && !hwgs::test::Date(ctx.test) &&
                            (frame_psm_code == hwgs::psm::kCt24 || (hwgs::frame::Fbmsk(ctx.frame) >> 24) == 0xFFu);
      // NX-2026-11-27-BN: off again. Run BM: no read-back was saved by it (the cause was another
      // one, see "superseded" below) and cars showed through walls in the race; a slot of the
      // other format at the same base can hold an image that memory no longer has.
      const auto psm_matches = [&](uint32_t slot_psm) {
        if (slot_psm == tpsm_code) return true;
        if (true) return false;
        const bool tex32 = tpsm_code == hwgs::psm::kCt32 || tpsm_code == hwgs::psm::kCt24;
        const bool slot32 = slot_psm == hwgs::psm::kCt32 || slot_psm == hwgs::psm::kCt24;
        return rgb_only && tex32 && slot32;
      };
      if (kDirectRtt && !active_hit && (tpsm_code == hwgs::psm::kCt32 || ct24_ok || ct24_const || ct16_ok || ct16_raw || alpha_dead) && twms <= 2u &&
          twmt <= 2u && ttw <= kDkSurface && tth <= kDkSurface) {
        for (int i = 0; i < kColourSlots && direct_rtt < 0; ++i) {
          const Surface &o = s.colour[i];
          if (i != s.colour_active && o.valid && o.view.basePointer == hwgs::tex0::Tbp0(tex0) &&
              o.fbw == hwgs::tex0::Tbw(tex0) && psm_matches(o.psm_code)) {
            direct_rtt = i;
            direct_ct24 = tpsm_code != hwgs::psm::kCt32 && !(ct16_raw && !ct16_ok); // CT24 / CT16: TCC=0
            path = "direct";
          }
        }
      }
      // NX-2026-11-27-AU: the texture is the active target itself and nothing else holds unflushed
      // pixels under it: sample a snapshot instead of writing back.
      if (kDirectRtt && direct_rtt < 0 && active_hit && s.snap.mem && tex_hit_mask == kSlotBit(s.colour_active) &&
          (tpsm_code == hwgs::psm::kCt32 || ct24_ok || ct24_const || ct16_ok || ct16_raw || alpha_dead) && twms <= 2u && twmt <= 2u &&
          ttw <= kDkSurface && tth <= kDkSurface) {
        const Surface &a = s.active_colour();
        if (a.view.basePointer == hwgs::tex0::Tbp0(tex0) && a.fbw == hwgs::tex0::Tbw(tex0) && a.psm_code == tpsm_code) {
          self_snap = true;
          direct_ct24 = tpsm_code != hwgs::psm::kCt32 && !(ct16_raw && !ct16_ok);
          path = "snap";
        }
      }
      // NX-2026-11-27-AP: race screen copy (3D buffer 0x1a40 sampled into a 16-bit display
      // target). Turns the R3D traces on; on the 20th and 60th copy lists every slot and dumps
      // the 3D buffer (kind 4, 640x448) as the GPU holds it.
      if (direct_rtt >= 0 && is_race3d(s.colour[direct_rtt]) && frame_psm_code == hwgs::psm::kCt16 && fbw == 10u) {
        static unsigned copy_n = 0;
        ++copy_n;
        g_race_trace = false; // NX-2026-11-27-AX: R3D traces off
        if (g_race_trace && (copy_n == 20u || copy_n == 60u)) {
          for (int i = 0; i < kColourSlots; ++i) {
            const Surface &c = s.colour[i];
            if (!c.valid) continue;
            std::fprintf(stderr,
                         "[hw-dk] R3D slots copy=%u: %d%s base=%05x fbw=%u psm=%02x seeded (%d,%d)-(%d,%d) dirty "
                         "(%d,%d)-(%d,%d) stamp=%llu dstamp=%llu\n",
                         copy_n, i, i == s.colour_active ? "*" : "", static_cast<unsigned>(c.view.basePointer), c.fbw,
                         c.psm_code, c.seeded.x0, c.seeded.y0, c.seeded.x1, c.seeded.y1, c.dirty.x0, c.dirty.y0, c.dirty.x1,
                         c.dirty.y1, static_cast<unsigned long long>(c.stamp), static_cast<unsigned long long>(c.dirty_stamp));
          }
          // GPU content: read the whole 640x448 back without touching SLM or slot state.
          Surface &src3d = s.colour[direct_rtt];
          s.begin_recording();
          s.barrier();
          DkImageView v3;
          dkImageViewDefaults(&v3, &src3d.image);
          DkImageRect r3{0, 0, 0, 640, 448, 1};
          DkCopyBuf d3{dkMemBlockGetGpuAddr(s.rb_mem), 0, 0};
          dkCmdBufCopyImageToBuffer(s.cmdbuf, &v3, &r3, &d3, 0);
          s.submit_and_wait(stats_);
          dkMemBlockFlushCpuCache(s.rb_mem, 0, (640u * 448u * 4u + 0xFFFu) & ~0xFFFu);
          dump_record(4, copy_n, 0, 0, 640, 448, reinterpret_cast<const std::uint32_t *>(s.rb_cpu));
          // And what SLM holds for the same rectangle (kind 5).
          std::vector<std::uint32_t> slm(640u * 448u);
          local.readRect(src3d.view, 0, 0, 640, 448, slm.data());
          dump_record(5, copy_n, 0, 0, 640, 448, slm.data());
        }
      }
      // NX-2026-11-27-AE: car buffer snapshots for gt4_dump.bin (kind 2), taken on the 40th, 120th
      // and 200th copy of the car buffer (0x2700) into 0x1e40.
      if (direct_rtt >= 0 && s.colour[direct_rtt].view.basePointer == 0x2700u && frame_view.basePointer == 0x1e40u) {
        static unsigned car_copy_n = 0;
        ++car_copy_n;
        if (car_copy_n == 40u || car_copy_n == 120u || car_copy_n == 200u) {
          std::string derr;
          flush_surfaces(s, stats_, local, kSlotBit(direct_rtt), derr);
          invalidate_flushed(s, stats_);
          std::vector<std::uint32_t> px(640u * 224u);
          local.readRect(s.colour[direct_rtt].view, 0, 0, 640, 224, px.data());
          dump_record(2, car_copy_n, 0, 0, 640, 224, px.data());
        }
      }
    }
    if (direct_rtt < 0 && !self_snap && colour_tex_hit) {
      // NX-2026-11-27-S: the texture reads GPU-drawn pixels SLM does not have yet. Write them
      // back before decoding, or the decode samples stale SLM (car-select black screen).
      std::string err;
      if (!flush_surfaces(s, stats_, local, tex_hit_mask, err)) {
        std::fprintf(stderr, "[hw-dk] tex-hit flush failed: %s\n", err.c_str());
      }
      ++stats_.tex_hit_flushes;
      // NX-2026-11-27-BM: always on (was behind the race trace switch): one line per distinct
      // cause, at most 40 in a session, now listing every valid colour slot (h = hit, * = active).
      {
        static std::vector<std::uint64_t> seen_causes;
        const std::uint64_t id = (tex0 & 0xFFFFFFFFFull) ^ (static_cast<std::uint64_t>(tex_hit_mask) << 40);
        if (seen_causes.size() < 40u && std::find(seen_causes.begin(), seen_causes.end(), id) == seen_causes.end()) {
          seen_causes.push_back(id);
          std::fprintf(stderr,
                       "[hw-dk] R3D hit: tex tbp=%05x tbw=%u psm=%02x %ux%u clamp=%011llx fst=%d nv=%u tex_blocks=%u | frame "
                       "base=%05x fbw=%u psm=%02x | hit slots:",
                       static_cast<unsigned>(hwgs::tex0::Tbp0(tex0)), static_cast<unsigned>(hwgs::tex0::Tbw(tex0)),
                       static_cast<unsigned>(hwgs::tex0::Psm(tex0)), ttw, tth,
                       static_cast<unsigned long long>(ctx.clamp & 0xfffffffffffull), bit(prim, hwgs::primbit::kFst) ? 1 : 0,
                       static_cast<unsigned>(batch.vertices.size()), tex_blocks,
                       static_cast<unsigned>(frame_view.basePointer), fbw, frame_psm_code);
          for (int i = 0; i < kColourSlots; ++i) {
            const Surface &c = s.colour[i];
            if (!c.valid) continue;
            std::fprintf(stderr, " [%d%s%s %05x/%u/%02x dirty (%d,%d)-(%d,%d) seeded (%d,%d)-(%d,%d)]", i,
                         (tex_hit_mask & kSlotBit(i)) ? "h" : "", i == s.colour_active ? "*" : "",
                         static_cast<unsigned>(c.view.basePointer), c.fbw, c.psm_code, c.dirty.x0, c.dirty.y0, c.dirty.x1,
                         c.dirty.y1, c.seeded.x0, c.seeded.y0, c.seeded.x1, c.seeded.y1);
          }
          std::fprintf(stderr, " | abe=%d test=%05llx fbmsk=%08x texa=%010llx tcc=%u tfx=%u", abe ? 1 : 0,
                       static_cast<unsigned long long>(ctx.test & 0xFFFFFull), static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)),
                       static_cast<unsigned long long>(batch.state.env.texa & 0xFFFFFFFFFFull),
                       static_cast<unsigned>(hwgs::tex0::Tcc(tex0)), static_cast<unsigned>(hwgs::tex0::Tfx(tex0)));
          std::fprintf(stderr, "\n");
        }
      }
      {
        int first_slot = 0;
        while (first_slot < kColourSlots && !(tex_hit_mask & kSlotBit(first_slot))) ++first_slot;
        ++g_hit_hist[static_cast<std::uint32_t>(hwgs::tex0::Tbp0(tex0)) | (hwgs::tex0::Psm(tex0) << 16) |
                     (static_cast<std::uint32_t>(first_slot) << 24)];
      }
      invalidate_flushed(s, stats_); // NX-2026-11-27-T: targeted
      path = "flush";
    } else if (direct_rtt < 0 && s.dz().dirty_overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks)) {
      // Depth is never read back; drop cached texels over the depth rows only.
      invalidate_textures_overlapping(s, hwgs::tex0::Tbp0(tex0), tex_blocks, stats_); // NX-2026-11-27-T
      path = "depth";
    }
    // NX-2026-11-27-AB: slot the diagnostics below describe: the directly sampled one, else the
    // first non-active slot whose surface covers the texture, else the active slot.
    int diag_oi = direct_rtt;
    if (diag_oi < 0) {
      diag_oi = s.colour_active;
      for (int i = 0; i < kColourSlots; ++i) {
        if (i != s.colour_active && s.colour[i].valid && s.colour[i].overlaps(hwgs::tex0::Tbp0(tex0), tex_blocks)) {
          diag_oi = i;
          break;
        }
      }
    }
    // NX-2026-11-27-T: rate-limited diagnostic for textures that touch GPU-drawn memory.
    static unsigned diag_n = 0;
    static unsigned diag16_n = 0;
    const bool diag16 = frame_psm_code == hwgs::psm::kCt16 && diag16_n < 60;
    if (std::strcmp(path, "decode") != 0 && (diag_n < 64 || diag16)) {
      ++diag_n;
      if (diag16) ++diag16_n;
      const Surface &a = s.active_colour();
      std::fprintf(stderr,
                   "[hw-dk] T-tex %s: tex psm=%02x tbp=%05x tbw=%u %ux%u wm=%u/%u | frame base=%05x fbw=%u psm=%02x | "
                   "slot%d base=%05x fbw=%u psm=%02x | fbmsk=%08x ztst=%u\n",
                   path, hwgs::tex0::Psm(tex0), hwgs::tex0::Tbp0(tex0), hwgs::tex0::Tbw(tex0), ttw, tth, twms, twmt,
                   static_cast<unsigned>(a.view.basePointer), a.fbw, a.psm_code, diag_oi,
                   static_cast<unsigned>(s.colour[diag_oi].view.basePointer),
                   s.colour[diag_oi].fbw, s.colour[diag_oi].psm_code,
                   static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)), static_cast<unsigned>(hwgs::test::Ztst(ctx.test)));
    }
    // NX-2026-11-27-AE: blend factor of the car layer over the display, every 30th such draw.
    if (hwgs::tex0::Tbp0(tex0) == 0x1e40u && hwgs::tex0::Tbw(tex0) == 10u && hwgs::tex0::Psm(tex0) == hwgs::psm::kCt32) {
      static unsigned fade_n = 0;
      if ((fade_n++ % 30u) == 0u) {
        std::fprintf(stderr, "[hw-dk] fade: n=%u fix=%02x alpha=%010llx abe=%d fb=%05x\n", fade_n,
                     static_cast<unsigned>((ctx.alpha >> 32) & 0xFFu),
                     static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), abe ? 1 : 0,
                     static_cast<unsigned>(frame_view.basePointer));
      }
    }
    // NX-2026-11-27-X: car-select fade chain trace (own cap). Fires for every textured draw that
    // reads or writes one of the car-phase buffers (0x2700 = FBP 0x138, 0x1e40 = FBP 0xf2,
    // 0x2fc0 = FBP 0x17e) and prints why direct sampling was or was not taken, both slot states,
    // ALPHA and the SLM texel at the texture centre (post flush, i.e. what a decode will see).
    {
      static unsigned car_n = 0;
      const uint32_t tbp = hwgs::tex0::Tbp0(tex0);
      const uint32_t fbase = static_cast<uint32_t>(frame_view.basePointer);
      auto watched = [](uint32_t b) { return b == 0x2700u || b == 0x1e40u || b == 0x2fc0u; };
      if (car_n < 400 && (watched(tbp) || watched(fbase))) {
        ++car_n;
        const int oi = diag_oi;
        const Surface &a = s.active_colour();
        const Surface &o = s.colour[oi];
        const bool active_hit = a.dirty_overlaps(tbp, tex_blocks);
        uint32_t centre = 0;
        const auto tpsm = memory::decodePsm(hwgs::tex0::Psm(tex0));
        if (tpsm) {
          const memory::BufferView tv{tbp, hwgs::tex0::Tbw(tex0), *tpsm};
          centre = local.readPixel(tv, std::min(ttw / 2u, 320u), std::min(tth / 2u, 112u));
        }
        std::fprintf(stderr,
                     "[hw-dk] X-car #%u %s: tex tbp=%05x tbw=%u psm=%02x %ux%u wm=%u/%u tfx=%u tcc=%u | frame base=%05x "
                     "fbw=%u psm=%02x | hit=%d active_hit=%d | A%d base=%05x fbw=%u psm=%02x v=%d dirty=%d seeded=%d | "
                     "O%d base=%05x fbw=%u psm=%02x v=%d dirty=%d seeded=%d | alpha=%010llx abe=%d | slm_centre=%08x\n",
                     car_n, path, tbp, hwgs::tex0::Tbw(tex0), hwgs::tex0::Psm(tex0), ttw, tth, twms, twmt,
                     static_cast<unsigned>(hwgs::tex0::Tfx(tex0)), static_cast<unsigned>(hwgs::tex0::Tcc(tex0)), fbase,
                     fbw, frame_psm_code, colour_tex_hit ? 1 : 0, active_hit ? 1 : 0, s.colour_active,
                     static_cast<unsigned>(a.view.basePointer), a.fbw, a.psm_code, a.valid ? 1 : 0, a.dirty.empty() ? 0 : 1,
                     a.seeded.empty() ? 0 : 1, oi, static_cast<unsigned>(o.view.basePointer), o.fbw, o.psm_code,
                     o.valid ? 1 : 0, o.dirty.empty() ? 0 : 1, o.seeded.empty() ? 0 : 1,
                     static_cast<unsigned long long>(ctx.alpha & 0xffffffffffull), abe ? 1 : 0, centre);
      }
    }
  }

  // NX-2026-11-27-Z: retarget no longer flushes the parked slot. If its unflushed pixels share
  // SLM with the area the active slot is about to seed, write them back first so the seed is
  // current. Only when the active slot actually needs seeding there.
  // NX-2026-11-27-AA: generalised to every other slot, and applied to the direct-sample source
  // too (its seed reads SLM just like the active slot's).
  auto settle = [&](int target, const Rect &need) -> bool {
    const Surface &t = s.colour[target];
    if (t.seeded.contains(need)) return true;
    const uint32_t blocks = t.rows_blocks(need.y1 + 1);
    unsigned mask = 0;
    for (int i = 0; i < kColourSlots; ++i) {
      if (i != target && s.colour[i].dirty_overlaps(t.view.basePointer, blocks)) mask |= kSlotBit(i);
    }
    if (mask == 0) return true;
    if (!flush_surfaces(s, stats_, local, mask, error)) return false;
    ++stats_.coherence_flushes;
    invalidate_flushed(s, stats_);
    return true;
  };
  // NX-2026-11-27-AX: one opaque sprite writes every pixel of `area` (no blending, no alpha or
  // destination alpha test, no colour mask, depth test off or ALWAYS), so nothing of the old
  // content survives there: the target is marked seeded without reading SLM and without first
  // writing back other slots that share the memory. GT4 starts its passes with such clears and
  // full-rectangle copies; each one cost a write-back of the aliasing scratch target plus a
  // reseed of the rows it had touched (car select: 9 write-backs and 26 reseeds per frame).
  bool seed_skip = false;
  {
    const uint32_t fm = static_cast<uint32_t>(hwgs::frame::Fbmsk(ctx.frame));
    const bool colour_all = frame_psm_code == hwgs::psm::kCt24 ? (fm & 0x00FFFFFFu) == 0u : fm == 0u;
    // NX-2026-11-27-AY: also a batch of sprites that tile the area (the race copies its 3D
    // buffer to the display as 20 columns of 32 pixels: one full-screen seed per frame).
    if ((prim & 7u) == 6u && (batch.vertices.size() == 2u || sprites_tile(batch, area)) && !abe && !hwgs::test::Ate(ctx.test) &&
        !hwgs::test::Date(ctx.test) && colour_all && (!hwgs::test::Zte(ctx.test) || hwgs::test::Ztst(ctx.test) == 1u)) {
      Surface &c = s.active_colour();
      if (c.seeded.empty() || area.contains(c.seeded)) {
        c.seeded = area;
        seed_skip = true;
      } else if (c.seeded.contains(area)) {
        seed_skip = true;
      } else if (c.seeded.x0 == area.x0 && c.seeded.x1 == area.x1 && area.y0 <= c.seeded.y1 + 1 &&
                 area.y1 >= c.seeded.y0 - 1) {
        c.seeded = unite(c.seeded, area);
        seed_skip = true;
      }
    }
  }
  // NX-2026-11-27-BN: the cover above overwrites every pixel of the pages it spans (when it runs
  // the full width of the target). Unflushed pixels another slot holds wholly inside those pages
  // are older and now overwritten on the PS2 too, so that slot's pending write-back is dropped: a
  // write-back of the active slot would have gone over it anyway (flush_surfaces writes in draw
  // order). Run BM: the 128-wide scratch target at the base of the 3D buffer (`01a40/2/00`) stayed
  // pending under the 3D buffer, so the full-screen self-blend of the 3D buffer could not use the
  // snapshot path and wrote both back, every frame, in car select and in the race.
  // NX-2026-11-27-BX: the game also draws into the memory of its Z buffer as a colour target (a
  // colour slot with the base and width of a depth slot exists in the race: `00000/10/01`). The
  // depth image is separate here and never sees that draw. Until BV a depth target lost its slot
  // (and so its depth) about once a frame, which hid this; with 16 slots nothing clears it any
  // more. When an opaque full cover is drawn over a depth slot's rows through a colour frame,
  // the slot's depth is dropped, so its next use starts from cleared depth. One line per case,
  // first 16, with the colour written (a non-zero colour would be a Z value we do not load).
  if (seed_skip) {
    for (auto &d : s.depth_slots) {
      if (!d.valid || d.seeded.empty()) continue;
      if (uses_depth && &d == &s.dz()) continue;
      if (d.view.basePointer != frame_view.basePointer || d.fbw != fbw) continue;
      const bool whole = area.contains(d.seeded);
      static unsigned zc_n = 0;
      if (whole && zc_n < 16u) { // NX-2026-11-27-BY: whole covers only (BX spent the lines on partial ones)
        ++zc_n;
        const auto &v1 = batch.vertices.back();
        std::fprintf(stderr,
                     "[hw-dk] z-as-colour #%u: frame %05x/%u/%02x area (%d,%d)-(%d,%d) over depth %05x/%u/%02x seeded "
                     "(%d,%d)-(%d,%d) whole=%d tme=%d rgba=%02x%02x%02x%02x fbmsk=%08x\n",
                     zc_n, static_cast<unsigned>(frame_view.basePointer), fbw, frame_psm_code, area.x0, area.y0, area.x1,
                     area.y1, static_cast<unsigned>(d.view.basePointer), d.fbw, d.psm_code, d.seeded.x0, d.seeded.y0,
                     d.seeded.x1, d.seeded.y1, whole ? 1 : 0, tme ? 1 : 0, v1.r, v1.g, v1.b, v1.a,
                     static_cast<unsigned>(hwgs::frame::Fbmsk(ctx.frame)));
      }
      if (whole) {
        d.seeded = {};
        d.dirty = {};
      }
    }
  }
  if (seed_skip) {
    Surface &c = s.active_colour();
    const int page_w = static_cast<int>(std::max<uint32_t>(c.fbw, 1u)) * 64;
    if (area.x0 <= 0 && area.x1 + 1 >= page_w) {
      const uint32_t rpp = c.rows_per_page();
      const uint32_t page_blocks = std::max<uint32_t>(c.fbw, 1u) * 32u;
      const uint32_t p0 = (static_cast<uint32_t>(std::max(area.y0, 0)) + rpp - 1u) / rpp;
      const uint32_t p1 = static_cast<uint32_t>(std::max(area.y1 + 1, 0)) / rpp; // one past the last whole page row
      if (p1 > p0) {
        const uint32_t lo = static_cast<uint32_t>(c.view.basePointer) + p0 * page_blocks;
        const uint32_t hi = static_cast<uint32_t>(c.view.basePointer) + p1 * page_blocks;
        for (int i = 0; i < kColourSlots; ++i) {
          Surface &o = s.colour[i];
          if (i == s.colour_active || !o.valid || o.dirty.empty()) continue;
          // NX-2026-11-27-BO: only a slot at the base of the covered target (the scratch target
          // the game parks at the start of its back buffers). Run BN also dropped 320-wide car
          // select targets that start inside one another; write-backs there went from 4 to 14 per
          // frame.
          if (o.view.basePointer != c.view.basePointer) continue;
          uint32_t ob = 0, on = 0;
          o.row_range(o.dirty.y0, o.dirty.y1, ob, on);
          if (on == 0u || ob < lo || ob + on > hi) continue;
          static unsigned shown = 0;
          if (shown < 12u) {
            ++shown;
            std::fprintf(stderr, "[hw-dk] superseded: slot %05x/%u/%02x dirty (%d,%d)-(%d,%d) under cover of %05x/%u/%02x rows %d-%d\n",
                         static_cast<unsigned>(o.view.basePointer), o.fbw, o.psm_code, o.dirty.x0, o.dirty.y0, o.dirty.x1,
                         o.dirty.y1, static_cast<unsigned>(c.view.basePointer), c.fbw, c.psm_code, area.y0, area.y1);
          }
          ++g_superseded;
          o.dirty = {};
        }
      }
    }
  }
  if (!seed_skip) {
    if (!settle(s.colour_active, area)) return false;
    cover_surface(s, stats_, s.active_colour(), area, local);
  }
  if (uses_depth) cover_surface(s, stats_, s.dz(), area, local);
  if (direct_rtt >= 0 || self_snap) {
    const int dtw = std::min(1 << hwgs::tex0::Tw(ctx.tex0), static_cast<int>(kDkSurface));
    const int dth = std::min(1 << hwgs::tex0::Th(ctx.tex0), static_cast<int>(kDkSurface));
    // NX-2026-11-27-AA: seed only the texels this batch can sample instead of the whole texture
    // rectangle (a 1024x1024 seed is a 4 MiB upload). Per axis: REGION_CLAMP -> [MIN, MAX];
    // CLAMP -> vertex UV bounds clamped to the texture; REPEAT -> bounds if they stay inside the
    // texture, else the whole axis. Any unusable q falls back to the whole texture.
    bool uv_ok = !batch.vertices.empty();
    float u0 = 0.f, u1 = 0.f, v0 = 0.f, v1 = 0.f;
    const bool fst = bit(prim, hwgs::primbit::kFst);
    bool first = true;
    for (const auto &v : batch.vertices) {
      float fu, fv;
      if (fst) {
        fu = static_cast<float>(v.u) / 16.f;
        fv = static_cast<float>(v.v) / 16.f;
      } else {
        if (!(v.q > 0.f)) { uv_ok = false; break; }
        fu = v.s / v.q * static_cast<float>(dtw);
        fv = v.t / v.q * static_cast<float>(dth);
      }
      if (!(fu > -65536.f && fu < 65536.f && fv > -65536.f && fv < 65536.f)) { uv_ok = false; break; }
      if (first) { u0 = u1 = fu; v0 = v1 = fv; first = false; }
      u0 = std::min(u0, fu); u1 = std::max(u1, fu);
      v0 = std::min(v0, fv); v1 = std::max(v1, fv);
    }
    // NX-2026-11-27-BC: a sprite samples only the texels of its part inside the drawn area. Car
    // select draws sprites much larger than the scissor from its 640x224 car buffer; the bounds
    // above then gave all 1024 rows, the buffer was seeded from SLM down to row 1023 (over the
    // textures and scratch targets stored after it) and every write to those cut the seed again:
    // run BB, 291 seeds of 3 MiB and 20 queue drains per frame.
    if (uv_ok && (prim & 7u) == 6u && !batch.vertices.empty() && (batch.vertices.size() & 1u) == 0u) {
      auto clip = [](float a, float b, float ta, float tb, float lo, float hi, float &o0, float &o1) {
        float x = ta, y = tb;
        if (a != b) {
          const float t0 = std::clamp((lo - a) / (b - a), 0.f, 1.f), t1 = std::clamp((hi - a) / (b - a), 0.f, 1.f);
          x = ta + t0 * (tb - ta);
          y = ta + t1 * (tb - ta);
        }
        o0 = std::min(x, y);
        o1 = std::max(x, y);
      };
      bool cfirst = true;
      float cu0 = 0.f, cu1 = 0.f, cv0 = 0.f, cv1 = 0.f;
      for (std::size_t i = 0; i + 1u < batch.vertices.size(); i += 2u) {
        const auto &p = batch.vertices[i], &q = batch.vertices[i + 1u];
        float pu, pv, qu, qv;
        if (fst) {
          pu = static_cast<float>(p.u) / 16.f;
          pv = static_cast<float>(p.v) / 16.f;
          qu = static_cast<float>(q.u) / 16.f;
          qv = static_cast<float>(q.v) / 16.f;
        } else {
          pu = p.s / p.q * static_cast<float>(dtw);
          pv = p.t / p.q * static_cast<float>(dth);
          qu = q.s / q.q * static_cast<float>(dtw);
          qv = q.t / q.q * static_cast<float>(dth);
        }
        float a0, a1, b0, b1;
        clip(static_cast<float>(p.x4) / 16.f, static_cast<float>(q.x4) / 16.f, pu, qu, static_cast<float>(area.x0),
             static_cast<float>(area.x1 + 1), a0, a1);
        clip(static_cast<float>(p.y4) / 16.f, static_cast<float>(q.y4) / 16.f, pv, qv, static_cast<float>(area.y0),
             static_cast<float>(area.y1 + 1), b0, b1);
        if (cfirst) {
          cu0 = a0; cu1 = a1; cv0 = b0; cv1 = b1;
          cfirst = false;
        }
        cu0 = std::min(cu0, a0); cu1 = std::max(cu1, a1);
        cv0 = std::min(cv0, b0); cv1 = std::max(cv1, b1);
      }
      if (!cfirst) {
        u0 = cu0; u1 = cu1; v0 = cv0; v1 = cv1;
      }
    }
    auto axis = [&](uint32_t mode, float lo, float hi, int size, uint32_t rmin, uint32_t rmax, int &o0, int &o1) {
      o0 = 0;
      o1 = size - 1;
      if (mode == 2u) {
        o0 = std::min(static_cast<int>(rmin), size - 1);
        o1 = std::min(static_cast<int>(rmax), size - 1);
        if (o1 < o0) { o0 = 0; o1 = size - 1; }
        return;
      }
      if (!uv_ok) return;
      const int a = static_cast<int>(std::floor(lo)) - 1, b = static_cast<int>(std::ceil(hi)) + 1;
      // NX-2026-11-27-BD: the wrap test used the bounds with their one-texel margin, so a REPEAT
      // batch whose coordinates start at 0.5 (a = -1) counted as wrapping and asked for the whole
      // axis: car select's 20-sprite copy of the 640x224 car buffer (TW = TH = 10, REPEAT) seeded
      // 1024 rows (run BC, `tall src`). Coordinates inside [0, size] do not wrap; with bilinear
      // filtering the margin texel past an edge wraps to the opposite edge, which is only read
      // when the coordinate lies within half a texel of that edge, so half a texel is kept clear.
      if (mode == 0u && (lo < 0.f || hi > static_cast<float>(size) || (a < 0 && lo < 0.5f) ||
                         (b > size - 1 && hi > static_cast<float>(size) - 0.5f)))
        return; // wraps: any texel may be read
      o0 = std::max(0, std::min(a, size - 1));
      o1 = std::max(0, std::min(b, size - 1));
    };
    Rect src;
    axis(hwgs::clampreg::Wms(ctx.clamp), u0, u1, dtw, static_cast<uint32_t>(hwgs::clampreg::Minu(ctx.clamp)),
         static_cast<uint32_t>(hwgs::clampreg::Maxu(ctx.clamp)), src.x0, src.x1);
    axis(hwgs::clampreg::Wmt(ctx.clamp), v0, v1, dth, static_cast<uint32_t>(hwgs::clampreg::Minv(ctx.clamp)),
         static_cast<uint32_t>(hwgs::clampreg::Maxv(ctx.clamp)), src.y0, src.y1);
    const int src_slot = self_snap ? s.colour_active : direct_rtt;
    {
      // NX-2026-11-27-BC: first draws that still ask for more than 512 source rows.
      static unsigned tall_logged = 0;
      if (src.y1 - src.y0 + 1 > 512 && tall_logged < 10u) {
        ++tall_logged;
        std::fprintf(stderr,
                     "[hw-dk] tall src #%u: rows %d-%d cols %d-%d | prim=%u nv=%u fst=%d uv_ok=%d uv %.1f,%.1f-%.1f,%.1f | "
                     "tex tbp=%05x %dx%d wm=%u/%u | area %d,%d-%d,%d | frame base=%05x fbw=%u\n",
                     tall_logged, src.y0, src.y1, src.x0, src.x1, static_cast<unsigned>(prim & 7u),
                     static_cast<unsigned>(batch.vertices.size()), fst ? 1 : 0, uv_ok ? 1 : 0, u0, v0, u1, v1,
                     static_cast<unsigned>(hwgs::tex0::Tbp0(ctx.tex0)), dtw, dth,
                     static_cast<unsigned>(hwgs::clampreg::Wms(ctx.clamp)), static_cast<unsigned>(hwgs::clampreg::Wmt(ctx.clamp)),
                     area.x0, area.y0, area.x1, area.y1, static_cast<unsigned>(frame_view.basePointer), fbw);
      }
    }
    if (!settle(src_slot, src)) return false;
    cover_surface(s, stats_, s.colour[src_slot], src, local);
    s.colour[src_slot].stamp = ++s.use_clock;
    s.begin_recording();
    s.barrier(); // render-target writes visible to the sampler
    if (self_snap) {
      static std::uint64_t snaps = 0;
      if ((++snaps % 20000u) == 1u)
        std::fprintf(stderr, "[hw-dk] self-sample snapshots: %llu\n", static_cast<unsigned long long>(snaps));
      DkImageView from, to;
      dkImageViewDefaults(&from, &s.colour[src_slot].image);
      dkImageViewDefaults(&to, &s.snap.image);
      const DkImageRect r{static_cast<uint32_t>(src.x0), static_cast<uint32_t>(src.y0), 0,
                          static_cast<uint32_t>(src.x1 - src.x0 + 1), static_cast<uint32_t>(src.y1 - src.y0 + 1), 1};
      dkCmdBufCopyImage(s.cmdbuf, &from, &r, &to, &r, 0);
      s.barrier();
    }
  }

  // Texture: decoded once per (TEX0, TEXA, CLUT) until local memory changes, uploaded into a slot.
  Params params{};
  int image_index = kWhiteSlot;
  VertexSetup vs;
  vs.tme = tme;
  vs.fst = bit(prim, hwgs::primbit::kFst);
  vs.flat = !bit(prim, hwgs::primbit::kIip);
  vs.y_sign = s.y_sign;
  if (tme && direct_ct24 && hwgs::tex0::Psm(ctx.tex0) == hwgs::psm::kCt24 && hwgs::tex0::Tcc(ctx.tex0) == 1u &&
      ((batch.state.env.texa >> 15) & 1u) == 0u) {
    const uint32_t tfx = hwgs::tex0::Tfx(ctx.tex0);
    vs.alpha_op = tfx == 0u ? 1 : tfx == 2u ? 3 : 2;
    vs.alpha_k = static_cast<uint32_t>(batch.state.env.texa & 0xFFu);
  }
  if (uses_depth) {
    vs.z_scale = z_scale(s.dz().psm_code);
    vs.z_max = z_max(s.dz().psm_code);
  }
  int sampler_index = 0;
  if (tme && (direct_rtt >= 0 || self_snap)) {
    const uint32_t tw = 1u << hwgs::tex0::Tw(ctx.tex0), th = 1u << hwgs::tex0::Th(ctx.tex0);
    image_index = self_snap ? kSnapDesc : kRttDescBase + direct_rtt;
    ++stats_.rtt_direct;
    vs.tex_w = vs.tex_h = static_cast<float>(kDkSurface);
    vs.st_scale_s = static_cast<float>(tw) / static_cast<float>(kDkSurface);
    vs.st_scale_t = static_cast<float>(th) / static_cast<float>(kDkSurface);
    const uint32_t wms = hwgs::clampreg::Wms(ctx.clamp), wmt = hwgs::clampreg::Wmt(ctx.clamp);
    const bool linear = hwgs::tex1::Mmag(ctx.tex1) != 0u;
    sampler_index = (linear ? 4 : 0) + 2 + 1;
    params.texfn[0] = 1.f;
    params.texfn[1] = static_cast<float>(hwgs::tex0::Tfx(ctx.tex0));
    params.texfn[2] = direct_ct24 ? 0.f : static_cast<float>(hwgs::tex0::Tcc(ctx.tex0));
    // CLAMP on a texture smaller than the image becomes REGION_CLAMP to the texture rectangle.
    // NX-2026-11-27-X: REPEAT becomes REGION_REPEAT (mask tw-1, fix 0) over the 1024 image; the
    // shader's texelFetch path handles mode 3 per axis, so the other axis keeps its own mode.
    params.wrap[0] = wms == 0u ? 3.f : 2.f;
    params.wrap[1] = wmt == 0u ? 3.f : 2.f;
    params.wrap[2] = vs.tex_w;
    params.wrap[3] = vs.tex_h;
    params.region[0] = wms == 2u ? static_cast<float>(hwgs::clampreg::Minu(ctx.clamp)) : wms == 0u ? static_cast<float>(tw - 1) : 0.f;
    params.region[1] = wms == 2u ? static_cast<float>(hwgs::clampreg::Maxu(ctx.clamp)) : wms == 0u ? 0.f : static_cast<float>(tw - 1);
    params.region[2] = wmt == 2u ? static_cast<float>(hwgs::clampreg::Minv(ctx.clamp)) : wmt == 0u ? static_cast<float>(th - 1) : 0.f;
    params.region[3] = wmt == 2u ? static_cast<float>(hwgs::clampreg::Maxv(ctx.clamp)) : wmt == 0u ? 0.f : static_cast<float>(th - 1);
  } else if (tme) {
    // NX-2026-11-27-AD: key on the palette contents (FNV-1a), see TexKey.
    std::uint32_t clut_hash = 0;
    if (hwgs::IsIndexedPsm(hwgs::tex0::Psm(ctx.tex0))) {
      const auto pal = clut.Decode(hwgs::tex0::Psm(ctx.tex0), hwgs::tex0::Cpsm(ctx.tex0), hwgs::tex0::Csa(ctx.tex0),
                                   batch.state.env.texa);
      clut_hash = 2166136261u;
      for (uint32_t i = 0; i < pal.count; ++i) clut_hash = (clut_hash ^ pal.entries[i]) * 16777619u;
      clut_hash |= 1u; // never 0: 0 means "no palette"
    }
    // NX-2026-11-27-AW: decodes and GPU slots are keyed by content, not by address. The address
    // key (TEX0, TEXA, palette) only remembers the hash of the source bytes; it is hashed again
    // when a write touched the texture. The content key is the format fields of TEX0 (TBP0 and
    // CBP removed) plus that hash. GT4 streams its textures into GS memory every frame and often
    // at a different address: run AV re-used 15% of decodes by address (1415 of 9573).
    const TexKey akey{ctx.tex0, batch.state.env.texa, clut_hash};
    if (s.tex_meta.size() > 8192u) s.tex_meta.clear();
    State::TexMeta &ameta = s.tex_meta[akey];
    bool meta_stale = ameta.src_hash == 0;
    if (!meta_stale && tex_written_since(s, ctx.tex0, ameta.clock)) {
      meta_stale = true;
      ++stats_.tex_targeted_drops;
    }
    if (meta_stale) {
      ProfScope prof_hash(g_prof.hash);
      ++g_prof.hashes;
      const std::uint64_t h = tex_source_hash(local, ctx.tex0) | 1u;
      if (h == ameta.src_hash) ++g_prof.revalid;
      ameta.src_hash = h;
      ameta.stale = false;
      ameta.clock = s.write_clock;
    }
    const std::uint64_t canon_tex0 = ctx.tex0 & ~(0x3FFFull | (0x3FFFull << 37));
    const TexKey key{canon_tex0, batch.state.env.texa, clut_hash, ameta.src_hash};
    uint32_t tw = 1u << hwgs::tex0::Tw(key.tex0);
    uint32_t th = 1u << hwgs::tex0::Th(key.tex0);
    const std::vector<std::uint32_t> *texels = nullptr;
    // NX-2026-11-27-AX: a GPU slot with this content key needs no texels at all. Run AW decoded
    // 21388 textures per window to upload 2934: the 12 MiB CPU copy cache was evicting entries
    // whose slots were still valid.
    // NX-2026-11-27-AY: looked up in State::slot_of (was a scan of all slots).
    int found_slot = -1;
    {
      const auto f = s.slot_of.find(key);
      if (f != s.slot_of.end()) {
        const TexSlot &t = s.tex_slots[f->second];
        if (t.occupied && t.key == key && t.w == tw && t.h == th) found_slot = f->second;
      }
    }
    const bool have_slot = found_slot >= 0;
    auto it = have_slot ? s.tex_cache.end() : s.tex_cache.find(key);
    bool fresh_decode = false;
    if (have_slot) {
      ++stats_.tex_cache_hits;
    } else if (it != s.tex_cache.end()) {
      texels = &it->second;
      ++stats_.tex_cache_hits;
    } else {
      (void)fresh_decode;
      std::vector<std::uint32_t> decoded;
      // Axis gate aligns with pixel::texture_axis_usable (log2 <= 10 → 1024). NX-2026-11-13-E:
      // do not soft-draw 1×1 white on TME reject — count Reject::Texture and skip the batch.
      if (!decode_texture(batch, local, clut, decoded, tw, th) || tw == 0 || th == 0 || tw > 1024 ||
          th > 1024) {
        log_tex_reject("decode", key.tex0, tw, th);
        ++stats_.rejects[static_cast<int>(Reject::Texture)];
        return true;
      }
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

    // NX-2026-11-27-AE: gt4_dump.bin kind 1: each distinct decoded texture drawn into the car
    // buffer (0x2700), first 40, at most 256x256.
    if (texels && frame_view.basePointer == 0x2700u && static_cast<std::uint64_t>(tw) * th <= 65536u) {
      static std::vector<std::uint64_t> seen;
      const std::uint64_t id = key.tex0 ^ (static_cast<std::uint64_t>(key.clut_gen) * 0x9E3779B97F4A7C15ull);
      if (seen.size() < 40u && std::find(seen.begin(), seen.end(), id) == seen.end()) {
        seen.push_back(id);
        const auto &v0 = batch.vertices.front();
        const std::uint64_t meta = static_cast<std::uint64_t>(v0.r) | (static_cast<std::uint64_t>(v0.g) << 8) |
                                   (static_cast<std::uint64_t>(v0.b) << 16) | (static_cast<std::uint64_t>(v0.a) << 24) |
                                   ((ctx.test & 0xFFFFFull) << 32) | (static_cast<std::uint64_t>(abe ? 1 : 0) << 52);
        dump_record(1, key.tex0, ctx.alpha, meta, tw, th, texels->data());
      }
    }

    // NX-2026-11-27-AG: gt4_dump.bin kind 3: 512x512 PSMT8 backdrop tiles (car select black bar),
    // first 8 distinct. a=TEX0, b=CLAMP, c as kind 1.
    if (texels && hwgs::tex0::Psm(key.tex0) == 0x13u && tw == 512u && th == 512u) {
      static std::vector<std::uint64_t> seen_tiles;
      const std::uint64_t id = key.tex0 ^ (static_cast<std::uint64_t>(key.clut_gen) * 0x9E3779B97F4A7C15ull);
      if (seen_tiles.size() < 8u && std::find(seen_tiles.begin(), seen_tiles.end(), id) == seen_tiles.end()) {
        seen_tiles.push_back(id);
        const auto &v0 = batch.vertices.front();
        const std::uint64_t meta = static_cast<std::uint64_t>(v0.r) | (static_cast<std::uint64_t>(v0.g) << 8) |
                                   (static_cast<std::uint64_t>(v0.b) << 16) | (static_cast<std::uint64_t>(v0.a) << 24) |
                                   ((ctx.test & 0xFFFFFull) << 32) | (static_cast<std::uint64_t>(abe ? 1 : 0) << 52);
        dump_record(3, key.tex0, ctx.clamp, meta, tw, th, texels->data());
      }
    }

    // NX-2026-11-27-AW: the key holds the content hash, so a slot with this key is current.
    int slot_i = found_slot;
    // NX-2026-11-27-AF: slot class by size. Small textures never evict a large slot and the
    // other way round.
    const uint32_t need_pitch =
        (tw * 4u + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
    // NX-2026-11-27-AY: smallest class that holds the texture; a texture too large for every
    // class keeps class 0 and is refused by the size test below.
    int cls = 0;
    for (int c = kTexClassCount - 1; c >= 0; --c) {
      if (static_cast<std::uint64_t>(need_pitch) * th <= kTexClasses[c].bytes) {
        cls = c;
        break;
      }
    }
    const int class_lo = tex_class_lo(cls);
    const int class_n = kTexClasses[cls].count;
    const uint32_t class_cap = kTexClasses[cls].bytes;
    if (slot_i < 0) {
      // Least recently used slot of the class that no queued draw refers to (an empty slot has
      // last_use 0 and wins).
      auto pick = [&]() {
        int best = -1;
        for (int n = 0; n < class_n; ++n) {
          const TexSlot &t = s.tex_slots[class_lo + n];
          if (t.used_since_wait) continue;
          const std::uint32_t age = t.occupied ? t.last_use : 0u;
          if (best < 0 || age < (s.tex_slots[best].occupied ? s.tex_slots[best].last_use : 0u)) best = class_lo + n;
        }
        return best;
      };
      slot_i = pick();
      if (slot_i < 0) {
        // Prefer dropping the oldest slot without waiting: overwrite and hope the prior draw finished.
        // A full WaitIdle here stalls the EE for milliseconds per unique texture (car select).
        // NX-2026-11-27-Q: all 16 slots are referenced by queued draws. Wait once (rare with 16 slots)
        // instead of overwriting texels/descriptors the GPU may still be reading (corrupt textures).
        s.submit_and_wait(stats_);
        slot_i = pick();
        if (slot_i < 0) slot_i = class_lo;
      }
      TexSlot &slot = s.tex_slots[slot_i];
      if (slot.occupied) {
        ++g_tex_evict[cls];
        // NX-2026-11-27-BB: which large texture is decoded again every frame (run BA: one slot of
        // the 4 MiB class re-used per frame in the race).
        static unsigned big_logged = 0;
        if (cls == 0 && big_logged < 12u) {
          ++big_logged;
          std::fprintf(stderr,
                       "[hw-dk] big tex #%u: tbp=%05x tbw=%u psm=%02x %ux%u | frame base=%05x fbw=%u psm=%02x | prim=%u nv=%u\n",
                       big_logged, static_cast<unsigned>(hwgs::tex0::Tbp0(ctx.tex0)),
                       static_cast<unsigned>(hwgs::tex0::Tbw(ctx.tex0)), static_cast<unsigned>(hwgs::tex0::Psm(ctx.tex0)), tw,
                       th, static_cast<unsigned>(frame_view.basePointer), fbw, frame_psm_code, static_cast<unsigned>(prim & 7u),
                       static_cast<unsigned>(batch.vertices.size()));
        }
        const auto old = s.slot_of.find(slot.key);
        if (old != s.slot_of.end() && old->second == slot_i) s.slot_of.erase(old);
        slot.occupied = false;
      }
      const uint32_t pitch =
          (tw * 4u + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
      if (static_cast<std::uint64_t>(pitch) * th <= class_cap) {
        std::uint8_t *dst = s.tex_cpu + slot.mem_off;
        ProfScope prof_texup(g_prof.texup);
        ++g_prof.texups;
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
        slot.stale = false;
        s.slot_of[key] = slot_i;
        stats_.upload_bytes += static_cast<std::uint64_t>(tw) * th * 4u;
      } else {
        log_tex_reject("slot-size", key.tex0, tw, th);
        ++stats_.rejects[static_cast<int>(Reject::Texture)];
        return true; // pitch overflow: skip TME batch (no white soft-fail)
      }
    }
    if (slot_i < 0) {
      ++stats_.rejects[static_cast<int>(Reject::Texture)];
      return true; // defensive: never bind kWhiteSlot for a textured batch
    }
    s.tex_slots[slot_i].used_since_wait = true;
    s.tex_slots[slot_i].last_use = ++s.tex_clock;
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

  // NX-2026-11-27-BJ: one buffer kept between batches (was a heap allocation per batch). Only the
  // GS thread draws.
  static std::vector<GpuVert> verts;
  verts.clear();
  verts.reserve(batch.vertices.size() * 3);
  expand_prims(batch, vs, verts);
  if (verts.empty()) return true;
  if (verts.size() > s.vert_capacity) {
    ++stats_.rejects[static_cast<int>(Reject::Other)];
    return true; // drop oversized batch rather than soft-draw
  }
  if (s.vert_offset + verts.size() > s.vert_capacity || (s.recording && s.list_ops >= kMaxListOps)) {
    s.submit(stats_);
    s.submit_and_wait(stats_); // vertex ring is single-buffered; must reclaim before overwrite
  }
  const bool lines = batch.prims.front().type == hwgs::PrimType::Line ||
                     batch.prims.front().type == hwgs::PrimType::LineStrip;

  const uint32_t base = s.vert_offset;
  std::memcpy(s.data_cpu + base * sizeof(GpuVert), verts.data(), verts.size() * sizeof(GpuVert));
  s.vert_offset = base + static_cast<uint32_t>(verts.size());

  s.begin_recording();
  s.list_ops += 3u; // a batch drawn twice (GpuBlend::twice) and its state changes
  s.max_list_ops = std::max(s.max_list_ops, s.list_ops);
  DkImageView colour_view, depth_view;
  dkImageViewDefaults(&colour_view, &s.active_colour().image);
  dkImageViewDefaults(&depth_view, &s.dz().image);
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
  // NX-2026-11-27-Q: honour FBMSK per channel. Before this, masked passes (alpha-only writes, used by
  // car select / dealer scenes) wrote black RGB over the frame. A byte counts as masked when all of its
  // stored bits are masked (CT16: top 5 bits, alpha bit 31); partial byte masks stay approximate.
  uint32_t fb_chan = DkColorMask_RGBA;
  {
    const uint32_t fbmsk = hwgs::frame::Fbmsk(ctx.frame);
    const uint32_t fpsm = hwgs::frame::Psm(ctx.frame);
    const bool psm16 = fpsm == hwgs::psm::kCt16 || fpsm == hwgs::psm::kCt16S;
    auto byte_masked = [&](int shift) {
      const uint32_t b = (fbmsk >> shift) & 0xFFu;
      return psm16 ? (b & 0xF8u) == 0xF8u : b == 0xFFu;
    };
    if (byte_masked(0)) fb_chan &= ~uint32_t(DkColorMask_R);
    if (byte_masked(8)) fb_chan &= ~uint32_t(DkColorMask_G);
    if (byte_masked(16)) fb_chan &= ~uint32_t(DkColorMask_B);
    const bool alpha_masked = psm16 ? ((fbmsk >> 31) & 1u) != 0u : byte_masked(24);
    if (alpha_masked || fpsm == hwgs::psm::kCt24) fb_chan &= ~uint32_t(DkColorMask_A);
  }
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
    dkColorWriteStateSetMask(&cws, 0, (write_colour ? (write_alpha ? uint32_t(DkColorMask_RGBA) : uint32_t(DkColorMask_RGB)) : 0u) & fb_chan);
    dkCmdBufBindColorWriteState(s.cmdbuf, &cws);
    params.test[2] = invert ? 1.f : 0.f;
    dkCmdBufPushConstants(s.cmdbuf, dkMemBlockGetGpuAddr(s.ubo_mem), kParamsBytes, 0, sizeof(Params), &params);
    dkCmdBufDraw(s.cmdbuf, lines ? DkPrimitive_Lines : DkPrimitive_Triangles, static_cast<uint32_t>(verts.size()), 1, 0, 0);
  };
  // NX-2026-11-27-AF: first of two draws for an Ad blend. Colour only: alpha must stay as it is
  // (it is the coefficient of the second draw) and depth must not move (a GREATER test would
  // reject the second draw).
  if (gb.enable && gb.twice) draw_pass(false, true, false, false);
  draw_pass(false, true, true, depth.write);
  if (ap.second_pass) draw_pass(true, ap.second_colour, ap.second_alpha, ap.second_depth && depth.write);

  const bool colour_written = fb_chan != 0u;
  const bool depth_written = uses_depth && depth.write;
  if (colour_written) {
    Surface &cs = s.active_colour();
    const Rect before = cs.dirty;
    cs.dirty = unite(cs.dirty, area);
    cs.dirty_stamp = cs.stamp;
    // NX-2026-11-27-U: dirty is a bbox union. Log when it first spans more than one 448-row
    // field buffer, so stderr shows whether growth (not real overlap) drives tex-hit flushes.
    static std::uint64_t dirty_grow_logs = 0;
    if ((before.empty() || before.y1 - before.y0 < 448) && cs.dirty.y1 - cs.dirty.y0 >= 448 && dirty_grow_logs < 40) {
      ++dirty_grow_logs;
      std::fprintf(stderr,
                   "[hw-dk] dirty-grow: slot=%d base=0x%x fbw=%u psm=%u before=(%d,%d)-(%d,%d) area=(%d,%d)-(%d,%d) "
                   "dirty=(%d,%d)-(%d,%d) scissor=(%d,%d)-(%d,%d) tme=%d\n",
                   s.colour_active, static_cast<unsigned>(cs.view.basePointer), cs.fbw, cs.psm_code,
                   before.x0, before.y0, before.x1, before.y1, area.x0, area.y0, area.x1, area.y1,
                   cs.dirty.x0, cs.dirty.y0, cs.dirty.x1, cs.dirty.y1, scissor.x0, scissor.y0, scissor.x1,
                   scissor.y1, tme ? 1 : 0);
    }
  }
  if (depth_written) s.dz().dirty = unite(s.dz().dirty, area);
  if (uses_depth) ++stats_.depth_batches;
  stats_.batches++;
  stats_.draws++;
  stats_.vertices += verts.size();
  return true;
#endif
}

} // namespace hwgs::dk
