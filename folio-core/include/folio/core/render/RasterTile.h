// folio/core/render/RasterTile.h
//
// The one pixel container shared by the rasterizer, the cache and the
// compositor. Split out of RasterCache.h so that folio-engine can produce
// tiles without depending on the cache, and so the compositor can consume
// them without depending on either.
//
// MEMORY FORMAT: BGRA8888, premultiplied, bottom-up = false.
//
// That is PDFium's FPDFBitmap_BGRA laid out exactly as it writes it, which is
// also what Skia calls kBGRA_8888_SkColorType and what the Qt scene graph
// wants for a texture upload. Choosing PDFium's native layout means the whole
// pipeline from FPDF_RenderPageBitmapWithMatrix to the GPU is free of format
// conversion - there is no swizzle pass anywhere, on any platform. Changing
// this constant is a cross-cutting decision, not a local one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/TileKey.h"

namespace folio::core {

struct RasterTile {
    TileKey                key{};
    std::int32_t           width = 0;
    std::int32_t           height = 0;
    std::size_t            stride = 0;      // bytes per row, >= width * 4
    Generation             generation = 0;  // snapshot it was rendered from
    bool                   opaque = true;   // page had no transparency
    std::vector<std::byte> pixels;

    [[nodiscard]] std::size_t byteSize() const noexcept {
        return pixels.size() + sizeof(RasterTile);
    }

    /// Row pointer. Bounds-unchecked on purpose: this is on the composite
    /// path and the caller already knows the height.
    [[nodiscard]] const std::byte* row(std::int32_t y) const noexcept {
        return pixels.data() + static_cast<std::size_t>(y) * stride;
    }
    [[nodiscard]] std::byte* row(std::int32_t y) noexcept {
        return pixels.data() + static_cast<std::size_t>(y) * stride;
    }

    /// Allocate a correctly sized, zeroed tile.
    [[nodiscard]] static std::unique_ptr<RasterTile> make(
        const TileKey& key, std::int32_t w, std::int32_t h, Generation gen) {
        auto t = std::make_unique<RasterTile>();
        t->key = key;
        t->width = w;
        t->height = h;
        t->stride = static_cast<std::size_t>(w) * 4;
        t->generation = gen;
        t->pixels.resize(t->stride * static_cast<std::size_t>(h));
        return t;
    }
};

using TileHandle = std::shared_ptr<const RasterTile>;

}  // namespace folio::core
