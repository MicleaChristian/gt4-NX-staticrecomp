// SPDX-License-Identifier: MIT
//
// HWR CPU reference: local memory sink. Implements the front end's TransferSink on top of the
// M2 shadow local memory (memory::LocalMemory), so transfers land in the same SLM the
// rasterizer reads and writes.
// Copyright 2026 GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec): chapters 02.7, 10.2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "hw_memory.hpp"
#include "hw_transfers.hpp"

namespace hwgs::cpuref {

class LocalMemorySink final : public TransferSink {
public:
    LocalMemorySink();

    memory::LocalMemory& Local() { return local_; }
    const memory::LocalMemory& Local() const { return local_; }

    // TransferSink
    void InvalidateDestination(const UploadCommit& commit) override;
    void CommitUpload(const UploadCommit& commit, const uint8_t* bytes, size_t byteCount) override;
    void LocalToLocalMove(const LocalMoveCommit& move) override;
    void BeginLocalToHost(const LocalToHostSetup& setup) override;
    void CancelLocalToHost() override { ResetRead(); }

    // Performs the pending LOCAL->HOST read and returns its host bytes (empty when none is
    // pending or the format is illegal). The pending read is consumed.
    std::vector<uint8_t> TakeLocalToHostData();
    std::vector<uint8_t> ReadLocalToHost(size_t bytes, bool& overrun);
    void ResetRead();
    bool ReadDrained() const { return !pending_ && readCursor_ == readData_.size(); }
    bool LocalToHostPending() const { return pending_.has_value(); }

    // Optional: called before a transfer reads or writes local memory, with a conservative
    // block range (start block, block count; wraps at 16384). A drawer that keeps pixels
    // outside local memory (the Switch GPU writeback) uses it to write them back first.
    using AccessHook = std::function<void(uint32_t block, uint32_t blocks, bool write)>;
    void SetAccessHook(AccessHook hook) { accessHook_ = std::move(hook); }
    // NX-2026-11-27-AS: a read of local memory that is not a transfer (a palette load). Same hook, so a
    // drawer that keeps pixels on the GPU writes them back before the read.
    void NotifyRead(uint32_t block, uint32_t blocks) {
        if (accessHook_) accessHook_(block, blocks, false);
    }

    // Counters for the replay summary.
    uint64_t uploadCount = 0;
    uint64_t moveCount = 0;
    uint64_t readCount = 0;

private:
    void NotifyAccess(uint32_t block, uint32_t widthUnits, uint32_t rows, bool write, uint32_t psm);
    // NX-2026-11-27-BE: a write of the pixel rectangle (x, y, w, h) of a buffer.
    void NotifyWrite(uint32_t block, uint32_t widthUnits, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    memory::LocalMemory local_;
    AccessHook accessHook_;
    std::optional<LocalToHostSetup> pending_;
    std::vector<uint8_t> readData_;
    size_t readCursor_ = 0;
};

}  // namespace hwgs::cpuref
