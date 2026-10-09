// folio/core/engine/IPdfRasterizer.h
//
// The rendering half of the engine facade. Implemented by PdfiumRasterizer.
//
// CONTRACT: PDFium's public header states none of its APIs are thread-safe and
// they must be called from a single thread. Every method below is therefore
// called ONLY from that document's dedicated PDFium thread. The scheduler
// marshals; callers never touch this directly.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/RasterTile.h"
#include "folio/core/render/TileKey.h"

namespace folio::core {

struct RectI {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
};

struct PageInfo {
    float widthPt = 0.f;
    float heightPt = 0.f;
    int   rotation = 0;   // 0/90/180/270, from /Rotate
    bool  hasTransparency = false;
};

enum class Priority : std::uint8_t {
    VisibleTile    = 0,  // highest
    VisibleText    = 1,
    PrefetchTile   = 2,
    VisibleThumb   = 3,
    OffscreenThumb = 4,
    BackgroundText = 5,  // lowest
};

struct TileRequest {
    PageId      page = 0;

    /// Grid coordinates. CARRIED EXPLICITLY, not derived from tileDevice.
    ///
    /// They used to be derived, as `tileDevice.x / kTileSize`, which is
    /// correct exactly as long as every tile origin is a multiple of the tile
    /// size. Bleed (TileGrid::bledTile) broke that assumption the first time
    /// it was used: a bled rect starts at tx*512 - 24, and integer division
    /// maps both tx=0 and tx=1 onto 0. The scheduler then deduped two
    /// different tiles into one and three quarters of the page never
    /// rendered.
    ///
    /// The lesson is the usual one: if a value is known at the call site,
    /// pass it, rather than reconstructing it from a representation that is
    /// about to grow a special case.
    std::uint16_t tileX = 0;
    std::uint16_t tileY = 0;

    RectI       tileDevice{};      // device-pixel rect actually rasterised
    float       scale = 1.f;
    int         rotation = 0;
    ColorScheme scheme = ColorScheme::Light;
    Priority    priority = Priority::VisibleTile;
    Generation  snapshotGen = 0;   // stale requests are dropped on arrival
};

/// A finished tile, handed over by value.
///
/// The earlier design passed a std::span into a pooled buffer that was only
/// valid for the duration of the callback. It was genuinely zero-copy, and it
/// was wrong: the cache has to keep the pixels, so every tile paid a 1 MB
/// memcpy (~50 us) on the render thread to escape the pool. Transferring
/// ownership of the allocation instead costs one 1 MB mmap at render start
/// (~10 us) and nothing afterwards - the same buffer PDFium rendered into is
/// the buffer the cache stores and the compositor uploads.
///
/// A recycling pool would beat both, and is the obvious optimisation once the
/// profiler says allocation is the problem. It is not the problem today, and
/// a pool that hands out buffers still referenced by the GPU is a far worse
/// bug than an allocation.
struct TileResult {
    std::unique_ptr<RasterTile> tile;
    bool partial = false;              // progressive: more coming for this key

    [[nodiscard]] bool ok() const noexcept { return tile != nullptr; }
    [[nodiscard]] TileKey key() const noexcept { return tile ? tile->key : TileKey{}; }
};

/// Move-only argument, so the callback consumes the tile exactly once.
using TileCallback = std::function<void(TileResult)>;

class IPdfTextIndex;

class IPdfRasterizer {
public:
    virtual ~IPdfRasterizer() = default;

    /// Atomically re-point at a newer snapshot. The implementation opens the
    /// new FPDF_DOCUMENT in the background and keeps serving from the old one
    /// until the new is warm, then swaps. Never blocks the caller.
    virtual void setSnapshot(SnapshotPtr snapshot) = 0;

    /// Enqueue a tile. Completion arrives on the callback, possibly several
    /// times if progressive rendering yields partial results.
    virtual void submit(TileRequest request, TileCallback onDone) = 0;

    /// Drop queued work for a page that is older than `olderThanGen`.
    /// Cancellation is near-free thanks to the IFSDK_PAUSE slicing.
    virtual void cancel(PageId page, Generation olderThanGen) = 0;

    [[nodiscard]] virtual PageInfo pageInfo(PageId page) const = 0;
    [[nodiscard]] virtual std::size_t pageCount() const = 0;

    /// nullptr until the background text pass has reached this page.
    [[nodiscard]] virtual const IPdfTextIndex* textIndex(PageId page) = 0;
};

}  // namespace folio::core
