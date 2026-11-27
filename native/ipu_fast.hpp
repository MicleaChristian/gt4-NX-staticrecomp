// Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
// TASK-027 (IN-490): the hot parts of the IPU video decoder with exactly the donor's results (scripts/prepare_ipu_fast.py
// puts them in a derived runtime/src/ipu/ipu.cpp; native/ipu_fast_fixture.cpp compares them with the donor's formulas).
// GT4 plays video behind its menus and the decoder took about half of the EE thread: the i5-7200U missed 16.7 ms there.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace gt4::ipu_fast {

// n bits (1 to 32) from bit position `bit` of the stream, most significant first; bytes past the end read as zero.
inline std::uint32_t read_bits(const std::uint8_t *data, std::size_t size, std::size_t bit, unsigned n) {
  if (n == 0) return 0;
  const std::size_t byte = bit >> 3;
  std::uint64_t value = 0;
  if (byte + 8 <= size) {
    std::memcpy(&value, data + byte, 8);
    value = __builtin_bswap64(value);
  } else {
    for (std::size_t i = 0; i < 8; ++i) value = (value << 8) | (byte + i < size ? data[byte + i] : 0u);
  }
  return static_cast<std::uint32_t>((value << (bit & 7u)) >> (64 - n));
}

// The donor's 8 x 8 inverse DCT in doubles: most coefficients of a video block are zero, and a zero adds nothing to a sum.
// Every output is the same sum in the same order, so the same double. `by_x` is cosines[x][u], `by_u` its transpose.
inline void idct(const double (&by_x)[8][8], const double (&by_u)[8][8], const int *in, std::int16_t *out) {
  // Only the first coefficient: cosines[x][0] is one value for every x, so all 64 outputs are the same sum.
  bool only_first = true;
  for (int i = 1; i < 64 && only_first; ++i) only_first = in[i] == 0;
  if (only_first) {
    const double row = 0.0 + by_u[0][0] * in[0];
    const double sum = 0.0 + by_x[0][0] * row;
    const std::int16_t value = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(sum + 0.5)), -256, 255));
    for (int i = 0; i < 64; ++i) out[i] = value;
    return;
  }
#if defined(__aarch64__)
  // NX-2026-11-27-BU: the same sums, two x at a time (NEON, 64-bit lanes). Each lane does the
  // multiplication and the addition the loops below do for that x, in the same order, as two
  // separate operations (no fused multiply-add), so each output is the same double. Run BT, video
  // menus at the stock clock: 260 to 390 macroblocks per frame cost 6.4 to 9.6 ms in the decoder.
  {
    float64x2_t nrows[8][4];
    bool nrow_used[8];
    for (int v = 0; v < 8; ++v) {
      nrow_used[v] = false;
      for (int u = 0; u < 8; ++u) {
        const int coefficient = in[v * 8 + u];
        if (coefficient == 0) continue;
        if (!nrow_used[v]) {
          for (int k = 0; k < 4; ++k) nrows[v][k] = vdupq_n_f64(0.0);
          nrow_used[v] = true;
        }
        const float64x2_t scale = vdupq_n_f64(static_cast<double>(coefficient));
        for (int k = 0; k < 4; ++k) nrows[v][k] = vaddq_f64(nrows[v][k], vmulq_f64(vld1q_f64(&by_u[u][k * 2]), scale));
      }
    }
    const float64x2_t half = vdupq_n_f64(0.5);
    const int32x4_t lo = vdupq_n_s32(-256), hi = vdupq_n_s32(255);
    for (int y = 0; y < 8; ++y) {
      float64x2_t sum[4] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0)};
      for (int v = 0; v < 8; ++v) {
        if (!nrow_used[v]) continue;
        const float64x2_t scale = vdupq_n_f64(by_x[y][v]);
        for (int k = 0; k < 4; ++k) sum[k] = vaddq_f64(sum[k], vmulq_f64(scale, nrows[v][k]));
      }
      // floor(sum + 0.5) as an integer, clamped to -256..255 (the sums are far inside 32 bits).
      int32x2_t part[4];
      for (int k = 0; k < 4; ++k) part[k] = vmovn_s64(vcvtq_s64_f64(vrndmq_f64(vaddq_f64(sum[k], half))));
      const int32x4_t a = vminq_s32(vmaxq_s32(vcombine_s32(part[0], part[1]), lo), hi);
      const int32x4_t b = vminq_s32(vmaxq_s32(vcombine_s32(part[2], part[3]), lo), hi);
      vst1q_s16(out + y * 8, vcombine_s16(vmovn_s32(a), vmovn_s32(b)));
    }
    return;
  }
#endif
  alignas(16) double rows[8][8];
  bool row_used[8];
  for (int v = 0; v < 8; ++v) {
    row_used[v] = false;
    for (int u = 0; u < 8; ++u) {
      const int coefficient = in[v * 8 + u];
      if (coefficient == 0) continue;
      if (!row_used[v]) {
        for (int x = 0; x < 8; ++x) rows[v][x] = 0.0;
        row_used[v] = true;
      }
      const double scale = coefficient;
      for (int x = 0; x < 8; ++x) rows[v][x] += by_u[u][x] * scale;
    }
  }
  for (int y = 0; y < 8; ++y) {
    alignas(16) double sum[8] = {};
    for (int v = 0; v < 8; ++v) {
      if (!row_used[v]) continue;
      const double scale = by_x[y][v];
      for (int x = 0; x < 8; ++x) sum[x] += scale * rows[v][x];
    }
    for (int x = 0; x < 8; ++x) {
      out[y * 8 + x] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(sum[x] + 0.5)), -256, 255));
    }
  }
}

// One macroblock (raw: 256 Y, 64 Cb, 64 Cr) to RGBA32 or RGBA16 pixels; returns the bytes written (at most 1024). Written
// row by row over plain arrays so the compiler vectorises it.
inline std::size_t convert(const std::uint8_t *raw, bool rgb16, int threshold0, int threshold1, std::uint8_t *pixels) {
  std::size_t size = 0;
  for (int y = 0; y < 16; ++y) {
    const std::uint8_t *luma = raw + y * 16;
    const std::uint8_t *cb_row = raw + 256 + (y / 2) * 8;
    const std::uint8_t *cr_row = raw + 320 + (y / 2) * 8;
    alignas(16) int lum[16], cb[16], cr[16], r[16], g[16], b[16], a[16];
    for (int x = 0; x < 16; ++x) {
      cb[x] = cb_row[x / 2] - 128;
      cr[x] = cr_row[x / 2] - 128;
    }
    for (int x = 0; x < 16; ++x) lum[x] = 149 * std::max(0, luma[x] - 16);
    for (int x = 0; x < 16; ++x) {
      r[x] = std::clamp((lum[x] + 204 * cr[x] + 64) >> 7, 0, 255);
      g[x] = std::clamp((lum[x] - 50 * cb[x] - 104 * cr[x] + 64) >> 7, 0, 255);
      b[x] = std::clamp((lum[x] + 258 * cb[x] + 64) >> 7, 0, 255);
    }
    for (int x = 0; x < 16; ++x) {
      const int largest = std::max(std::max(r[x], g[x]), b[x]);
      a[x] = largest < threshold0 ? 0 : largest < threshold1 ? 0x40 : 0x80;
    }
    if (rgb16) {
      alignas(16) std::uint16_t row[16];
      for (int x = 0; x < 16; ++x) {
        row[x] = static_cast<std::uint16_t>((r[x] >> 3) | ((g[x] >> 3) << 5) | ((b[x] >> 3) << 10) | (a[x] & 0x80 ? 0x8000 : 0));
      }
      std::memcpy(pixels + size, row, sizeof row);
      size += sizeof row;
    } else {
      alignas(16) std::uint32_t row[16];
      for (int x = 0; x < 16; ++x) {
        row[x] = static_cast<std::uint32_t>(r[x]) | (static_cast<std::uint32_t>(g[x]) << 8) |
                 (static_cast<std::uint32_t>(b[x]) << 16) | (static_cast<std::uint32_t>(a[x]) << 24);
      }
      std::memcpy(pixels + size, row, sizeof row);
      size += sizeof row;
    }
  }
  return size;
}

}  // namespace gt4::ipu_fast
