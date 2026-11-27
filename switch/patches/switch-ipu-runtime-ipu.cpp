#include "ipu/ipu.hpp"

#include "ipu/tables.hpp"
#include "dmac.hpp"
#include "hw.hpp"
#include "ipu_fast.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// NX-2026-11-27-BX: defined further down, with the other Switch perf counters.
extern "C" {
extern unsigned long long gt4_ipu_kind_ticks[16];  // host time in run_command by command code
extern unsigned long long gt4_ipu_csc_mb[2];       // CSC macroblocks: [0] byte-aligned stream, [1] not
}

namespace ps2::ipu {

namespace {

using namespace tables;

struct NeedData {};
struct BadCode {};

constexpr std::uint32_t kEcd = 0x4000u, kScd = 0x8000u, kReset = 0x40000000u;
constexpr std::size_t kFifoBytes = 128;

constexpr std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr std::uint8_t kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18, 20, 22,
                                              24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

struct Entry {
    std::uint8_t length = 0;
    std::uint32_t value = 0;
};

struct Lookup {
    unsigned bits = 0;
    std::vector<Entry> entries;
};

struct DctEntry {
    std::uint8_t length = 0;
    std::uint8_t run = 0;
    std::int16_t level = 0;
};

struct DctLookup {
    unsigned bits = 0;
    std::vector<DctEntry> entries;
};

template <std::size_t N>
Lookup build(const Vlc (&table)[N]) {
    Lookup l;
    for (const Vlc &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Vlc &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.value};
    }
    return l;
}

template <std::size_t N>
DctLookup build(const Dct (&table)[N]) {
    DctLookup l;
    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
    return l;
}

struct Tables {
    Lookup mbai = build(kMbAddressIncrement), mb_i = build(kMbTypeI), mb_p = build(kMbTypeP), mb_b = build(kMbTypeB);
    Lookup motion = build(kMotionCode), dmv = build(kDmVector), dc_luma = build(kDcSizeLuma), dc_chroma = build(kDcSizeChroma);
    Lookup cbp = build(kCodedBlockPattern);
    DctLookup dct0 = build(kDctTable0), dct1 = build(kDctTable1);
    double cosines[8][8];
    double cosines_by_u[8][8];  // IN-490: cosines[x][u] as [u][x]

    Tables() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                cosines_by_u[u][x] = cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
    }
};

// IN-490: an object of this file, not a function-local static with a check on every table read.
const Tables g_tables;

inline const Tables &tables() { return g_tables; }

enum Command { kBclr, kIdec, kBdec, kVdec, kFdec, kSetiq, kSetvq, kCsc, kPack, kSetth };

std::vector<std::uint8_t> g_in;
std::size_t g_bit = 0;
std::vector<std::uint8_t> g_out;
std::size_t g_out_pos = 0;

std::uint32_t g_ctrl = 0, g_result = 0, g_command = 0;
int g_last = -1;
bool g_busy = false, g_decoding = false, g_interrupt = true, g_starved = false;
int g_phase = 0;
std::uint32_t g_qsc = 0, g_count = 0;
bool g_first = true;
std::uint8_t g_intra_iq[64], g_nonintra_iq[64];
std::uint16_t g_vqclut[16];
std::uint16_t g_th0 = 0, g_th1 = 0;
std::int16_t g_dc[3];
std::uint64_t g_commands[16], g_stalls = 0, g_out_qwords = 0;

bool pull() {
    std::uint8_t q[16];
    if (!dmac::ipu_pull(q)) return false;
    g_in.insert(g_in.end(), q, q + 16);
    return true;
}

bool have(std::size_t bits) {
    // NX-2026-11-27-BW: the qwords the loop below would pull one by one, in one call.
    const std::size_t need = (g_bit + bits + 7) / 8;
    if (g_in.size() >= need) return true;
    const std::size_t want = (need - g_in.size() + 15) / 16;
    const std::size_t old = g_in.size();
    g_in.resize(old + want * 16);
    const std::size_t got = dmac::ipu_pull_n(g_in.data() + old, want);
    g_in.resize(old + got * 16);
    if (got == want) return true;
    while (g_in.size() * 8 < g_bit + bits)
        if (!pull()) return false;
    return true;
}

std::size_t available() { return g_in.size() * 8 > g_bit ? g_in.size() * 8 - g_bit : 0; }

std::uint32_t bits_at(std::size_t bit, unsigned n) { return gt4::ipu_fast::read_bits(g_in.data(), g_in.size(), bit, n); }

std::uint32_t peek(unsigned n) {
    if (!have(n)) throw NeedData{};
    return bits_at(g_bit, n);
}

std::uint32_t get(unsigned n) {
    const std::uint32_t v = peek(n);
    g_bit += n;
    return v;
}

void skip(unsigned n) {
    if (!have(n)) throw NeedData{};
    g_bit += n;
}

std::uint32_t peek_soft(unsigned n, std::size_t &avail) {
    have(n);
    avail = available();
    return bits_at(g_bit, n);
}

std::uint32_t decode(const Lookup &l) {
    std::size_t avail;
    const Entry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    return e.value;
}

std::uint32_t peek_now() {
    const unsigned n = static_cast<unsigned>(std::min<std::size_t>(32, available()));
    return n ? bits_at(g_bit, n) << (32 - n) : 0u;
}

void commit() {
    const std::size_t drop = std::min(g_bit / 128 * 16, g_in.size() / 16 * 16);
    g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(drop));
    g_bit -= drop * 8;
}

void top_up() {
    // NX-2026-11-27-BW: in one call (was one qword per call).
    if (g_in.size() >= kFifoBytes) return;
    const std::size_t want = (kFifoBytes - g_in.size() + 15) / 16;
    const std::size_t old = g_in.size();
    g_in.resize(old + want * 16);
    const std::size_t got = dmac::ipu_pull_n(g_in.data() + old, want);
    g_in.resize(old + got * 16);
}

unsigned dc_precision() { return (g_ctrl >> 16) & 3u; }
bool alternate_scan() { return g_ctrl & 0x100000u; }
bool intra_vlc_one() { return g_ctrl & 0x200000u; }
bool nonlinear_q() { return g_ctrl & 0x400000u; }
bool mpeg1() { return g_ctrl & 0x800000u; }
unsigned picture_type() { return (g_ctrl >> 24) & 7u; }

int read_dc_diff(int channel) {
    const unsigned size = decode(channel ? tables().dc_chroma : tables().dc_luma) & 0xFFFFu;
    if (!size) return 0;
    const int v = static_cast<int>(get(size));
    return v < (1 << (size - 1)) ? v + 1 - (1 << size) : v;
}

bool read_coefficient(const DctLookup &l, int &run, int &level) {
    std::size_t avail;
    const DctEntry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    if (e.run == kEscape) {
        run = static_cast<int>(get(6));
        if (mpeg1()) {
            int v = static_cast<int>(get(8));
            if (v == 0) v = static_cast<int>(get(8));
            else if (v == 128) v = static_cast<int>(get(8)) - 256;
            else if (v > 128) v -= 256;
            level = v;
        } else {
            const int v = static_cast<int>(get(12));
            level = v & 0x800 ? v - 0x1000 : v;
        }
        return true;
    }
    run = e.run;
    level = get(1) ? -e.level : e.level;
    return true;
}

void idct(const int *in, std::int16_t *out) { gt4::ipu_fast::idct(tables().cosines, tables().cosines_by_u, in, out); }

void decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {
    // IN-491: only the coded positions are read below, in the same rising order the loop over 64 visited them.
    int coef[64];
    std::uint8_t coded[64];
    int coded_count = 0;
    int index = 0;
    const DctLookup &table = intra && intra_vlc_one() ? tables().dct1 : tables().dct0;
    const bool table_one = &table == &tables().dct1;
    if (intra) {
        g_dc[channel] = static_cast<std::int16_t>(g_dc[channel] + read_dc_diff(channel));
        coef[0] = g_dc[channel];
        coded[coded_count++] = 0;
        index = 1;
    } else if (peek(1)) {
        coef[0] = get(2) & 1u ? -1 : 1;
        coded[coded_count++] = 0;
        index = 1;
    }
    for (;;) {
        int run = 0, level = 0;
        // IN-491: with 32 bits at hand one read answers the end of block, the code and its sign: three reads per coefficient
        // were the largest part of the decoder. Fewer bits, or an escape code, take the donor's way.
        if (available() >= 32) {
            const std::uint32_t window = bits_at(g_bit, 32);
            if (index && (table_one ? (window >> 28) == 6u : (window >> 30) == 2u)) {
                g_bit += table_one ? 4 : 2;
                break;
            }
            const DctEntry &e = table.entries[window >> (32 - table.bits)];
            if (!e.length) throw BadCode{};
            if (e.run != kEscape) {
                g_bit += e.length + 1u;
                run = e.run;
                level = (window >> (31 - e.length)) & 1u ? -e.level : e.level;
                index += run;
                if (index >= 64) throw BadCode{};
                coded[coded_count++] = static_cast<std::uint8_t>(index);
                coef[index++] = level;
                continue;
            }
        }
        if (index) {
            if (table_one ? peek(4) == 6u : peek(2) == 2u) {
                skip(table_one ? 4 : 2);
                break;
            }
        }
        read_coefficient(table, run, level);
        index += run;
        if (index >= 64) throw BadCode{};
        coded[coded_count++] = static_cast<std::uint8_t>(index);
        coef[index++] = level;
    }
    const std::uint8_t *scan = alternate_scan() ? kAlternate : kZigzag;
    const std::uint8_t *weights = intra ? g_intra_iq : g_nonintra_iq;
    const int scale = mpeg1() || !nonlinear_q() ? static_cast<int>(qsc) * 2 : kNonLinearScale[qsc & 31u];
    int f[64] = {};
    int sum = 0;
    for (int i = 0; i < coded_count; ++i) {
        const int k = coded[i];
        const int v = coef[k];
        const int r = scan[k];
        int x;
        if (intra && k == 0) {
            x = v * (8 >> dc_precision());
        } else {
            if (!v) continue;
            const int sign = v > 0 ? 1 : -1;
            x = intra ? (v * weights[r] * scale * 2) / 32 : ((2 * v + sign) * weights[r] * scale) / 32;
            if (mpeg1() && x && !(x & 1)) x -= sign;
        }
        x = std::clamp(x, -2048, 2047);
        f[r] = x;
        sum += x;
    }
    if (!mpeg1() && !(sum & 1)) f[63] += f[63] & 1 ? -1 : 1;
    idct(f, out);
}

void decode_macroblock(std::int16_t *raw, bool intra, bool field, bool dc_reset, std::uint32_t qsc) {
    const unsigned cbp = intra ? 0x3Fu : decode(tables().cbp) & 0xFFu;
    if (dc_reset) g_dc[0] = g_dc[1] = g_dc[2] = static_cast<std::int16_t>(128 << dc_precision());
    std::int16_t blocks[6][64] = {};
    for (int b = 0; b < 6; ++b)
        if (cbp & (32u >> b)) decode_block(blocks[b], b < 4 ? 0 : b - 3, intra, qsc);
    for (int i = 0; i < 8; ++i) {
        const int top = field ? 2 * i : i, bottom = field ? 2 * i + 1 : i + 8;
        std::memcpy(raw + top * 16, blocks[0] + i * 8, 16);
        std::memcpy(raw + top * 16 + 8, blocks[1] + i * 8, 16);
        std::memcpy(raw + bottom * 16, blocks[2] + i * 8, 16);
        std::memcpy(raw + bottom * 16 + 8, blocks[3] + i * 8, 16);
    }
    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}

void convert(const std::uint8_t *raw, bool rgb16) {
    std::uint8_t pixels[1024];
    const std::size_t size = gt4::ipu_fast::convert(raw, rgb16, g_th0, g_th1, pixels);
    g_out.insert(g_out.end(), pixels, pixels + size);
}

void emit_raw16(const std::int16_t *raw) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw);
    g_out.insert(g_out.end(), bytes, bytes + 768);
}

bool start_code_follows() {
    std::size_t avail;
    const std::uint32_t next = peek_soft(8, avail);
    return avail >= 8 && next == 0;
}

template <typename F>
bool atomic(F &&body) {
    const std::size_t bit = g_bit;
    std::int16_t dc[3];
    std::memcpy(dc, g_dc, sizeof dc);
    const std::size_t out = g_out.size();
    try {
        body();
        return true;
    } catch (const NeedData &) {
        ++g_stalls;
        g_bit = bit;
        std::memcpy(g_dc, dc, sizeof dc);
        g_out.resize(out);
        return false;
    }
}

bool run_idec() {
    const bool dtd = (g_command >> 24) & 1u, rgb16 = (g_command >> 27) & 1u;
    for (;;) {
        bool ok = true;
        switch (g_phase) {
        case 0:
            ok = atomic([&] { skip(g_command & 0x3Fu); });
            if (ok) g_phase = 1;
            break;
        case 1:
            ok = atomic([&] {
                const std::uint32_t type = decode(tables().mb_i) & 0xFFFFu;
                const bool field = dtd && get(1);
                std::uint32_t qsc = g_qsc;
                if (type & 0x10u) qsc = get(5);
                std::int16_t raw[384];
                decode_macroblock(raw, true, field, g_first, qsc);
                std::uint8_t raw8[384];
                for (int i = 0; i < 384; ++i) raw8[i] = static_cast<std::uint8_t>(std::clamp<int>(raw[i], 0, 255));
                convert(raw8, rgb16);
                g_qsc = qsc;
            });
            if (ok) {
                g_first = false;
                g_phase = 2;
            }
            break;
        case 2:
            ok = atomic([&] {
                std::size_t avail;
                const std::uint32_t next = peek_soft(8, avail);
                if (avail < 8) throw NeedData{};
                if (next == 0) {
                    g_bit = (g_bit + 7) & ~std::size_t{7};
                    g_phase = 3;
                    return;
                }
                std::uint32_t v;
                do v = decode(tables().mbai) & 0xFFFFu;
                while (v == 0x22u || v == 0x23u);
                g_phase = 1;
            });
            break;
        case 3:
            ok = atomic([&] {
                for (;;) {
                    std::size_t avail;
                    const std::uint32_t code = peek_soft(24, avail);
                    if (avail < 24 || code == 1) {
                        g_phase = 4;
                        return;
                    }
                    if (code != 0) throw BadCode{};
                    g_bit += 8;
                }
            });
            break;
        default:
            g_ctrl |= kScd;
            return true;
        }
        if (!ok) return false;
        commit();
    }
}

bool run_command() {
    const std::uint32_t cmd = g_command;
    switch (cmd >> 28) {
    case kBclr:
        g_in.clear();
        g_bit = cmd & 0x7Fu;
        return true;
    case kIdec:
        return run_idec();
    case kBdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            std::int16_t raw[384];
            decode_macroblock(raw, (cmd >> 27) & 1u, (cmd >> 25) & 1u, (cmd >> 26) & 1u, (cmd >> 16) & 0x1Fu);
            emit_raw16(raw);
        });
        if (!ok) return false;
        commit();
        if (start_code_follows()) {
            g_ctrl |= kScd;
            g_interrupt = false;
        }
        return true;
    }
    case kVdec: {
        // NX-2026-11-27-BL: as for FDEC, for the skipped bits.
        if (!have(cmd & 0x3Fu)) {
            ++g_stalls;
            return false;
        }
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            const Lookup *table = nullptr;
            switch ((cmd >> 26) & 3u) {
            case 0: table = &tables().mbai; break;
            case 1:
                switch (picture_type()) {
                case 1: table = &tables().mb_i; break;
                case 2: table = &tables().mb_p; break;
                case 3: table = &tables().mb_b; break;
                default: throw BadCode{};
                }
                break;
            case 2: table = &tables().motion; break;
            default: table = &tables().dmv; break;
            }
            g_result = decode(*table);
        });
        if (ok) commit();
        return ok;
    }
    case kFdec: {
        // NX-2026-11-27-BL: the same stall without a C++ exception (a throw costs several
        // microseconds on the Switch). have() pulls exactly what skip() and peek() would have
        // pulled before giving up, and nothing else is changed by a stalled FDEC.
        if (!have((cmd & 0x3Fu) + 32u)) {
            ++g_stalls;
            return false;
        }
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            g_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
    case kSetiq: {
        std::uint8_t *matrix = (cmd >> 27) & 1u ? g_nonintra_iq : g_intra_iq;
        std::uint8_t values[64];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint8_t &v : values) v = static_cast<std::uint8_t>(get(8));
        });
        if (!ok) return false;
        for (int k = 0; k < 64; ++k) matrix[kZigzag[k]] = values[k];
        commit();
        return true;
    }
    case kSetvq: {
        std::uint16_t values[16];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint16_t &v : values) v = static_cast<std::uint16_t>(get(16));
        });
        if (!ok) return false;
        std::memcpy(g_vqclut, values, sizeof values);
        commit();
        return true;
    }
    case kCsc:
        while (g_count) {
            std::uint8_t raw[384];
            const bool ok = atomic([&] {
                // IN-491: GT4 reassembles its pictures on the EE and sends them through CSC: 384 reads of 8 bits per
                // macroblock were half of the decoder. Bytes are bytes when the stream sits on a byte boundary.
                if ((g_bit & 7u) == 0) {
                    if (!have(384 * 8)) throw NeedData{};
                    std::memcpy(raw, g_in.data() + (g_bit >> 3), 384);
                    g_bit += 384 * 8;
                    ++gt4_ipu_csc_mb[0];
                } else {
                    // NX-2026-11-27-BX: the same 384 bytes get(8) returns, taken from byte pairs
                    // (a byte of the stream at bit offset s is the low 8 - s bits of one byte and
                    // the top s bits of the next). have() makes the last, partly used byte present.
                    if (!have(384 * 8)) throw NeedData{};
                    const std::uint8_t *p = g_in.data() + (g_bit >> 3);
                    const unsigned sh = static_cast<unsigned>(g_bit & 7u);
                    for (int i = 0; i < 384; ++i) raw[i] = static_cast<std::uint8_t>((p[i] << sh) | (p[i + 1] >> (8u - sh)));
                    g_bit += 384 * 8;
                    ++gt4_ipu_csc_mb[1];
                }
                convert(raw, (cmd >> 27) & 1u);
            });
            if (!ok) return false;
            commit();
            --g_count;
        }
        return true;
    case kSetth:
        g_th0 = static_cast<std::uint16_t>(cmd & 0x1FFu);
        g_th1 = static_cast<std::uint16_t>((cmd >> 16) & 0x1FFu);
        return true;
    default:
        std::fprintf(stderr, "[ipu] command 0x%08X is not implemented\n", cmd);
        return true;
    }
}

}  // namespace
}  // namespace ps2::ipu

// NX-2026-11-27-BL: host time inside a command write (IPU_CMD), split by step, for the Switch perf
// log. Index: 0 whole write, 1 run_command, 2 drain, 3 top_up. Calls by command code in
// gt4_ipu_cmds[16]. Run BK: 33,000 command writes per frame on the intro video at 450 ns each.
extern "C" {
unsigned long long gt4_ipu_ticks[4] = {};
unsigned long long gt4_ipu_cmds[16] = {};
unsigned long long gt4_ipu_stalls = 0;  // commands that ran out of stream (total)
unsigned long long gt4_ipu_kind_ticks[16] = {};
unsigned long long gt4_ipu_csc_mb[2] = {};
}

namespace ps2::ipu {
namespace {

inline unsigned long long prof_tick() {
#if defined(__aarch64__)
    unsigned long long v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

void drain() {
    // NX-2026-11-27-BW: in one call (was one qword per call).
    if (g_out_pos + 16 <= g_out.size()) {
        const std::size_t pushed = dmac::ipu_push_n(g_out.data() + g_out_pos, (g_out.size() - g_out_pos) / 16);
        g_out_pos += pushed * 16;
        g_out_qwords += pushed;
    }
    if (g_out_pos == g_out.size()) {
        g_out.clear();
        g_out_pos = 0;
    }
}

void step() {
    const unsigned long long t0 = prof_tick();
    if (g_busy && g_decoding) {
        try {
            g_starved = !run_command();
            if (!g_starved) g_decoding = false;
        } catch (const BadCode &) {
            std::fprintf(stderr, "[ipu] bad code in command 0x%08X\n", g_command);
            g_ctrl |= kEcd;
            g_decoding = false;
            g_busy = false;
        }
    }
    const unsigned long long t1 = prof_tick();
    gt4_ipu_kind_ticks[(g_command >> 28) & 15u] += t1 - t0;  // NX-2026-11-27-BX
    drain();
    if (g_busy && !g_decoding && g_out.empty()) {
        g_busy = false;
        if (g_interrupt) hw::intc_raise(8);
    }
    const unsigned long long t2 = prof_tick();
    top_up();
    const unsigned long long t3 = prof_tick();
    gt4_ipu_ticks[1] += t1 - t0;
    gt4_ipu_ticks[2] += t2 - t1;
    gt4_ipu_ticks[3] += t3 - t2;
    gt4_ipu_stalls = g_stalls;
}

}

void reset() {
    g_in.clear();
    g_out.clear();
    g_bit = g_out_pos = 0;
    g_ctrl = g_result = g_command = 0;
    g_last = -1;
    g_busy = g_decoding = false;
    std::memcpy(g_intra_iq, kDefaultIntra, 64);
    std::fill(std::begin(g_nonintra_iq), std::end(g_nonintra_iq), 16);
    std::memset(g_vqclut, 0, sizeof g_vqclut);
    g_th0 = g_th1 = 0;
    std::memset(g_dc, 0, sizeof g_dc);
}

std::uint32_t read32(std::uint32_t a) {
    switch (a & 0xFFFFFFFCu) {
    case 0x10002000u:
        return g_last == kVdec || g_last == kFdec ? g_result : peek_now();
    case 0x10002004u:
        return g_busy ? 0x80000000u : 0u;
    case 0x10002010u: {
        const std::uint32_t ifc = g_starved ? 0u : static_cast<std::uint32_t>(std::min<std::size_t>(8, g_in.size() / 16));
        const std::uint32_t ofc = static_cast<std::uint32_t>(std::min<std::size_t>(8, (g_out.size() - g_out_pos) / 16));
        return (g_ctrl & ~0x800000FFu) | (g_busy ? 0x80000000u : 0u) | ifc | (ofc << 4);
    }
    case 0x10002020u: {
        if (g_starved) return 0;
        const std::uint32_t bp = static_cast<std::uint32_t>(g_bit & 127u);
        std::uint32_t ifc = static_cast<std::uint32_t>(g_in.size() / 16 - std::min(g_in.size() / 16, g_bit / 128));
        std::uint32_t fp = 0;
        if (bp && ifc) {
            fp = 1;
            --ifc;
        }
        return (fp << 16) | (std::min<std::uint32_t>(ifc, 8) << 8) | bp;
    }
    case 0x10002030u:
        return g_busy ? 0u : peek_now();
    case 0x10002034u:
        return g_busy || available() < 32 ? 0x80000000u : 0u;
    default:
        return 0;
    }
}

void write32(std::uint32_t a, std::uint32_t v) {
    switch (a) {
    case 0x10002000u: {
    }
    {
        const unsigned long long cmd_t0 = prof_tick();
        ++gt4_ipu_cmds[v >> 28];
        struct CmdTimer {
            unsigned long long t0;
            ~CmdTimer() { gt4_ipu_ticks[0] += prof_tick() - t0; }
        } cmd_timer{cmd_t0};
        g_ctrl &= ~(kEcd | kScd);
        g_starved = false;
        ++g_commands[v >> 28];
        g_command = v;
        g_last = static_cast<int>(v >> 28);
        g_busy = g_decoding = g_interrupt = true;
        g_phase = 0;
        g_qsc = (v >> 16) & 0x1Fu;
        g_first = true;
        g_count = v & 0x7FFu;
        step();
        {
            static const char *trace = std::getenv("PS2_TRACE_IPU");
            static std::uint64_t traced = 0, from = trace ? std::strtoull(trace, nullptr, 10) : 0;
            if (trace && ++traced >= from && traced < from + 200000)
                std::fprintf(stderr, "[ipu] cmd 0x%08X -> result %08X ctrl %08X bit %zu next %08X%08X\n", v, g_result, g_ctrl, g_bit,
                             bits_at(g_bit, 32), bits_at(g_bit + 32, 32));
        }
    }
        break;
    case 0x10002010u:
        if (v & kReset) {
            g_starved = false;
            g_in.clear();
            g_out.clear();
            g_bit = g_out_pos = 0;
            g_busy = g_decoding = false;
            g_th0 = g_th1 = 0;
        }
        g_ctrl = (g_ctrl & ~0x3FFF0000u) | (v & 0x3FFF0000u);
        break;
    default:
        if (a >= 0x10007010u && a < 0x10007020u) {
            const std::uint8_t *bytes = reinterpret_cast<const std::uint8_t *>(&v);
            g_in.insert(g_in.end(), bytes, bytes + 4);
            step();
        }
        break;
    }
}

void write_fifo(const std::uint8_t *qword) {
    g_in.insert(g_in.end(), qword, qword + 16);
    step();
}

void read_fifo(std::uint8_t *qword) {
    if (g_out_pos + 16 <= g_out.size()) {
        std::memcpy(qword, g_out.data() + g_out_pos, 16);
        g_out_pos += 16;
    } else {
        std::memset(qword, 0, 16);
    }
    step();
}

void dma_started(int) { step(); }

void stats() {
    std::fprintf(stderr, "[ipu] commands:");
    for (int i = 0; i < 16; ++i)
        if (g_commands[i]) std::fprintf(stderr, " %X:%llu", i, static_cast<unsigned long long>(g_commands[i]));
    std::fprintf(stderr, " stalls %llu out %llu qwords, busy %d decoding %d cmd 0x%08X in %zu bytes bit %zu out %zu\n",
                 static_cast<unsigned long long>(g_stalls), static_cast<unsigned long long>(g_out_qwords), g_busy, g_decoding, g_command,
                 g_in.size(), g_bit, g_out.size() - g_out_pos);
}

}

namespace ps2::ipu {

void state(ps2::state::Stream &s) {
    s.tag("ipu");
    s.vec(g_in);
    s.size(g_bit);
    s.vec(g_out);
    s.size(g_out_pos);
    s.pod(g_ctrl);
    s.pod(g_result);
    s.pod(g_command);
    s.pod(g_last);
    s.pod(g_busy);
    s.pod(g_decoding);
    s.pod(g_interrupt);
    s.pod(g_starved);
    s.pod(g_phase);
    s.pod(g_qsc);
    s.pod(g_count);
    s.pod(g_first);
    s.pod(g_intra_iq);
    s.pod(g_nonintra_iq);
    s.pod(g_vqclut);
    s.pod(g_th0);
    s.pod(g_th1);
    s.pod(g_dc);
}

}
