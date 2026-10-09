// folio/core/render/TileKey.h
//
// 16-byte cache key. Deliberately packed so it hashes to a single cache line
// and so TileKey can be compared with a single 128-bit memcmp.
//
// The important design decision here is scaleQ: zoom is QUANTISED to 1/8 steps
// before it enters the key. A pinch gesture generates hundreds of distinct
// scales but only ~8 cache generations, which is the difference between a
// cache that works and one that thrashes (architecture plan §8.4).

#pragma once

#include <cstdint>
#include <functional>

namespace folio::core {

using PageId = std::uint32_t;

enum class ColorScheme : std::uint8_t {
    Light = 0,
    Sepia = 1,
    Dark  = 2,
};

/// Zoom quantisation step. 1/8 => 12.5% buckets.
inline constexpr float kScaleQuantum = 0.125f;

[[nodiscard]] inline std::uint16_t quantiseScale(float scale) noexcept {
    const float q = scale / kScaleQuantum;
    const float r = (q < 0.f) ? 0.f : q;
    return static_cast<std::uint16_t>(r + 0.5f);
}

[[nodiscard]] inline float dequantiseScale(std::uint16_t q) noexcept {
    return static_cast<float>(q) * kScaleQuantum;
}

struct TileKey {
    PageId        pageId = 0;    // 4
    std::uint16_t tileX = 0;     // 2
    std::uint16_t tileY = 0;     // 2
    std::uint16_t scaleQ = 0;    // 2  quantised, never raw float
    std::uint8_t  rotation = 0;  // 1  0/1/2/3 == 0/90/180/270
    ColorScheme   scheme = ColorScheme::Light;  // 1
    std::uint32_t pad = 0;       // 4  explicit, keeps the struct hashable

    friend bool operator==(const TileKey&, const TileKey&) = default;
};

static_assert(sizeof(TileKey) == 16, "TileKey must stay 16 bytes");

/// Tile edge in device pixels. 512 balances render granularity against
/// per-call PDFium overhead; see plan §8.1.
inline constexpr std::int32_t kTileSize = 512;

/// Proxy level: always-resident low-res page image so fast scrolling never
/// shows blank pages. ~256 KB per page.
inline constexpr std::int32_t kProxyLongEdge = 256;

}  // namespace folio::core

template <>
struct std::hash<folio::core::TileKey> {
    std::size_t operator()(const folio::core::TileKey& k) const noexcept {
        // FNV-1a over the 16 bytes. Keys are dense and well-distributed.
        const auto* p = reinterpret_cast<const std::uint8_t*>(&k);
        std::size_t h = 1469598103934665603ULL;
        for (std::size_t i = 0; i < sizeof(k); ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
};
