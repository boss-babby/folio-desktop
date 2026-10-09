// folio-engine/pdfium/SnapshotFileAccess.h  --  work package B2
//
// Bridges DocumentSnapshot's rope into PDFium's FPDF_FILEACCESS.
//
// This is why PDFium never needs a temp file and never sees a flattened copy
// of the document: it reads straight out of the rope segments, so a save that
// appends a 4 KB incremental update costs 4 KB, not a re-serialisation of a
// 200 MB file (plan §5.2).

#pragma once

#include "folio/core/engine/DocumentSnapshot.h"

#include <fpdfview.h>

#include <span>

namespace folio::engine {

using folio::core::DocumentSnapshot;
using folio::core::SnapshotPtr;

/// Owns a snapshot reference for as long as PDFium might read from it.
/// Lifetime rule: this must outlive the FPDF_DOCUMENT created from it.
/// PDFium reads lazily - a dangling m_Param here is a use-after-free that
/// surfaces as random parse errors much later, so the shared_ptr is load-bearing.
class SnapshotFileAccess {
public:
    explicit SnapshotFileAccess(SnapshotPtr snapshot)
        : snapshot_(std::move(snapshot)) {
        access_.m_FileLen  = static_cast<unsigned long>(snapshot_->size());
        access_.m_GetBlock = &SnapshotFileAccess::getBlock;
        access_.m_Param    = this;
    }

    SnapshotFileAccess(const SnapshotFileAccess&) = delete;
    SnapshotFileAccess& operator=(const SnapshotFileAccess&) = delete;

    [[nodiscard]] FPDF_FILEACCESS* handle() noexcept { return &access_; }
    [[nodiscard]] const SnapshotPtr& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] folio::core::Generation generation() const noexcept {
        return snapshot_->generation();
    }

private:
    /// PDFium contract: return 1 on full read, 0 on failure. A short read is
    /// a failure, not a partial success.
    static int getBlock(void* param, unsigned long pos,
                        unsigned char* buf, unsigned long size) {
        auto* self = static_cast<SnapshotFileAccess*>(param);
        const std::size_t got = self->snapshot_->read(
            static_cast<std::size_t>(pos),
            std::span<std::byte>(reinterpret_cast<std::byte*>(buf), size));
        return got == size ? 1 : 0;
    }

    SnapshotPtr     snapshot_;
    FPDF_FILEACCESS access_{};
};

}  // namespace folio::engine
