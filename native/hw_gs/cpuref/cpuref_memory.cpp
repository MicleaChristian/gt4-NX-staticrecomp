// SPDX-License-Identifier: MIT
// Copyright 2026 GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec): chapters 02.7, 10.2.
#include "cpuref_memory.hpp"

#include <algorithm>
#include <chrono>

// NX-2026-11-27-BC: upload totals for the Switch GPU backend's time line.
extern "C" {
unsigned long long gt4_gs_upload_ns = 0;
unsigned long long gt4_gs_upload_bytes = 0;
unsigned long long gt4_gs_uploads = 0;
}

#include "hw_batch.hpp"

namespace hwgs::cpuref {

namespace {

// TRXPOS.DIR (two bits) to the memory model's direction enum.
memory::TransferDirection DirectionOf(uint32_t dir) {
    return static_cast<memory::TransferDirection>(dir & 3u);
}

}  // namespace

LocalMemorySink::LocalMemorySink() = default;

// NX-2026-11-27-AZ: block footprint by pixel format. FootprintBlocks assumes the CT32 page shape
// (64x32 pixels) for every format, so a PSMT8 transfer (128x64 per page) was reported four times
// too large and a PSMT4 one (128x128) eight times. On the Switch GPU backend those phantom blocks
// reached the render target stored after the texture: every texture upload next to the car
// buffer forced a write-back and a full reseed of it (run AY, car select: 77 write-backs and
// 121 seeds of 3 MiB per 60 frames).
void LocalMemorySink::NotifyAccess(uint32_t block, uint32_t widthUnits, uint32_t rows, bool write, uint32_t psm) {
    if (!accessHook_) return;
    uint32_t pageRows = 32u, unitsPerPage = 1u;
    switch (psm) {
        case 0x13u: pageRows = 64u; unitsPerPage = 2u; break;   // PSMT8
        case 0x14u: pageRows = 128u; unitsPerPage = 2u; break;  // PSMT4
        case 0x02u:
        case 0x0Au:
        case 0x32u:
        case 0x3Au: pageRows = 64u; break;                      // CT16, CT16S, Z16, Z16S
        default: break;                                         // 32-bit page shape
    }
    const uint64_t pages = std::max<uint32_t>((std::max<uint32_t>(widthUnits, 1u) + unitsPerPage - 1u) / unitsPerPage, 1u);
    const uint64_t pageLines = std::max<uint32_t>((rows + pageRows - 1u) / pageRows, 1u);
    const uint32_t blocks = static_cast<uint32_t>(std::min<uint64_t>(pages * pageLines * 32u, 16384u));
    accessHook_(block, std::min(blocks, FootprintBlocks(widthUnits, rows)), write);
}

// NX-2026-11-27-BE: what the write being announced covers, for the Switch GPU backend's access
// hook (valid only during the hook call). The hook wrote every GPU-held surface under a write back
// to local memory first, a wait for the GPU each time (run BD: 6 per race frame, 20 per frame on
// car select, 2 to 4 ms each); with this it can tell when the write replaces everything the
// surface still holds and skip the write-back. `exact_blocks` is non-zero when the rectangle is
// whole pages, so the block range [exact_first, exact_first + exact_blocks) is written in full.
// The reported range now starts at the page row of `y`, not at the buffer's first row.
extern "C" {
struct Gt4GsWriteInfo {
    unsigned valid, base, bw, psm, x0, y0, w, h, page_rows, page_w, exact_first, exact_blocks;
};
Gt4GsWriteInfo gt4_gs_write_info = {};
}

void LocalMemorySink::NotifyWrite(uint32_t block, uint32_t widthUnits, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                  uint32_t h) {
    if (!accessHook_) return;
    uint32_t pageRows = 32u, unitsPerPage = 1u;
    switch (psm) {
        case 0x13u: pageRows = 64u; unitsPerPage = 2u; break;   // PSMT8
        case 0x14u: pageRows = 128u; unitsPerPage = 2u; break;  // PSMT4
        case 0x02u:
        case 0x0Au:
        case 0x32u:
        case 0x3Au: pageRows = 64u; break;                      // CT16, CT16S, Z16, Z16S
        default: break;                                         // 32-bit page shape
    }
    const uint32_t units = std::max<uint32_t>(widthUnits, 1u);
    const uint32_t perRow = std::max<uint32_t>(units / unitsPerPage, 1u);  // pages per page row, as LocalMemory addresses
    const uint64_t pagesCeil = std::max<uint32_t>((units + unitsPerPage - 1u) / unitsPerPage, 1u);
    const uint64_t endRows = std::max<uint32_t>((y + h + pageRows - 1u) / pageRows, 1u);
    const uint64_t total =
        std::min<uint64_t>(std::min<uint64_t>(pagesCeil * endRows * 32u, 16384u), FootprintBlocks(widthUnits, y + h));
    uint64_t offset = static_cast<uint64_t>(y / pageRows) * perRow * 32u;
    if (offset >= total) offset = 0;
    Gt4GsWriteInfo info{};
    info.valid = 1;
    info.base = block;
    info.bw = widthUnits;
    info.psm = psm;
    info.x0 = x;
    info.y0 = y;
    info.w = w;
    info.h = h;
    info.page_rows = pageRows;
    info.page_w = unitsPerPage * 64u;
    if (x == 0u && units % unitsPerPage == 0u && w == perRow * unitsPerPage * 64u && y % pageRows == 0u && h != 0u &&
        h % pageRows == 0u) {
        const uint64_t first = static_cast<uint64_t>(block) + static_cast<uint64_t>(y / pageRows) * perRow * 32u;
        const uint64_t count = static_cast<uint64_t>(h / pageRows) * perRow * 32u;
        if (first + count <= 16384u) {
            info.exact_first = static_cast<unsigned>(first);
            info.exact_blocks = static_cast<unsigned>(count);
        }
    }
    gt4_gs_write_info = info;
    // NX-2026-11-27-BS: a 32-bit or 24-bit rectangle inside the first page of its buffer touches
    // only the blocks its 8x8 cells map to (the GS block order inside a page, same table as
    // hw_memory.cpp kBlockTable8x4); report that range, not the whole page. Run BR, race: the two
    // write-backs left per frame were uploads of 8x2 and 32x16 pixels at blocks 0x3de2 and 0x3eb0,
    // reported as 32 blocks each and so "reaching" the render targets at 0x3e00 and 0x3ec0, which
    // they do not touch (about 10 ms of GPU wait apiece).
    if ((psm == 0x00u || psm == 0x01u) && w != 0u && h != 0u && x + w <= 64u && y + h <= 32u) {
        static const uint8_t kTable[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                             {2, 3, 6, 7, 18, 19, 22, 23},
                                             {8, 9, 12, 13, 24, 25, 28, 29},
                                             {10, 11, 14, 15, 26, 27, 30, 31}};
        uint32_t lo = 31u, hi = 0u;
        for (uint32_t by = y / 8u; by <= (y + h - 1u) / 8u; ++by) {
            for (uint32_t bx = x / 8u; bx <= (x + w - 1u) / 8u; ++bx) {
                lo = std::min<uint32_t>(lo, kTable[by][bx]);
                hi = std::max<uint32_t>(hi, kTable[by][bx]);
            }
        }
        if (static_cast<uint64_t>(block) + hi < 16384u) {
            accessHook_(block + lo, hi - lo + 1u, true);
            gt4_gs_write_info.valid = 0;
            return;
        }
    }
    accessHook_(static_cast<uint32_t>((block + offset) & 16383u), static_cast<uint32_t>(total - offset), true);
    gt4_gs_write_info.valid = 0;
}

void LocalMemorySink::InvalidateDestination(const UploadCommit& commit) {
    // The CPU reference has no caches to drop: the SLM is always the truth.
    NotifyWrite(commit.dbp, commit.dbw, commit.dpsm, commit.dsax, commit.dsay, commit.rrw, commit.rrh);
}

void LocalMemorySink::CommitUpload(const UploadCommit& commit, const uint8_t* bytes, size_t byteCount) {
    NotifyWrite(commit.dbp, commit.dbw, commit.dpsm, commit.dsax, commit.dsay, commit.rrw, commit.rrh);
    const auto psm = memory::decodePsm(commit.dpsm);
    if (!psm) {
        return;  // invalid PSM: the transfer is dropped (counted by the front end's rules)
    }
    const memory::BufferView destination{commit.dbp, commit.dbw, *psm};
    const memory::TransferRect rect{commit.dsax, commit.dsay, commit.rrw, commit.rrh};
    const auto upload_t0 = std::chrono::steady_clock::now();
    memory::hostToLocal(local_, destination, rect, std::span<const uint8_t>(bytes, byteCount));
    gt4_gs_upload_ns += static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - upload_t0).count());
    gt4_gs_upload_bytes += byteCount;
    ++gt4_gs_uploads;
    ++uploadCount;
}

void LocalMemorySink::LocalToLocalMove(const LocalMoveCommit& move) {
    const auto sourcePsm = memory::decodePsm(move.spsm);
    const auto destinationPsm = memory::decodePsm(move.dpsm);
    if (!sourcePsm || !destinationPsm) {
        return;
    }
    NotifyAccess(move.sbp, move.sbw, move.ssay + move.rrh, false, move.spsm);
    NotifyWrite(move.dbp, move.dbw, move.dpsm, move.dsax, move.dsay, move.rrw, move.rrh);
    memory::localToLocal(local_,
                         memory::BufferView{move.sbp, move.sbw, *sourcePsm}, move.ssax, move.ssay,
                         memory::BufferView{move.dbp, move.dbw, *destinationPsm}, move.dsax, move.dsay,
                         move.rrw, move.rrh, DirectionOf(move.dir));
    ++moveCount;
}

void LocalMemorySink::BeginLocalToHost(const LocalToHostSetup& setup) {
    ResetRead();
    pending_ = setup;
    ++readCount;
}

void LocalMemorySink::ResetRead() {
    pending_.reset();
    readData_.clear();
    readCursor_ = 0;
}

std::vector<uint8_t> LocalMemorySink::ReadLocalToHost(size_t bytes, bool& overrun) {
    if (pending_) {
        readData_ = TakeLocalToHostData();
        readCursor_ = 0;
    }
    const size_t available = readData_.size() - readCursor_;
    const size_t copied = std::min(bytes, available);
    overrun = copied < bytes;
    std::vector<uint8_t> result(bytes, 0);
    std::copy_n(readData_.begin() + static_cast<std::ptrdiff_t>(readCursor_), copied, result.begin());
    readCursor_ += copied;
    return result;
}

std::vector<uint8_t> LocalMemorySink::TakeLocalToHostData() {
    std::vector<uint8_t> data;
    if (!pending_) {
        return data;
    }
    const LocalToHostSetup setup = *pending_;
    pending_.reset();
    const auto psm = memory::decodePsm(setup.spsm);
    if (!psm) {
        return data;
    }
    NotifyAccess(setup.sbp, setup.sbw, setup.ssay + setup.rrh, false, setup.spsm);
    const memory::BufferView source{setup.sbp, setup.sbw, *psm};
    const memory::TransferRect rect{setup.ssax, setup.ssay, setup.rrw, setup.rrh};
    memory::localToHost(local_, source, rect, data);  // 4-bit sources are refused (data stays empty)
    return data;
}

}  // namespace hwgs::cpuref
