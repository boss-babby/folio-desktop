#include "shell/FrameEncoder.h"

#include "include/core/SkData.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"

namespace folio::shell {

std::vector<std::uint8_t> FrameEncoder::encode(SkSurface& surface, Codec codec,
                                               int quality) {
    SkPixmap pm;
    if (!surface.peekPixels(&pm)) return {};

    sk_sp<SkData> data;
    switch (codec) {
        case Codec::Jpeg: {
            SkJpegEncoder::Options opt;
            opt.fQuality = quality;
            // 4:2:0 chroma subsampling halves the bytes at a cost that is
            // invisible for UI chrome but WOULD be visible on coloured text,
            // so it is only acceptable because the page content we stream is
            // predominantly greyscale. A real remote-rendering product would
            // use 4:4:4 here.
            opt.fDownsample = SkJpegEncoder::Downsample::k420;
            data = SkJpegEncoder::Encode(pm, opt);
            break;
        }
        case Codec::Png: {
            SkPngEncoder::Options opt;
            opt.fZLibLevel = 3;  // speed over size; this is a live stream
            data = SkPngEncoder::Encode(pm, opt);
            break;
        }
        case Codec::Webp: {
            SkWebpEncoder::Options opt;
            opt.fQuality = static_cast<float>(quality);
            opt.fCompression = SkWebpEncoder::Compression::kLossy;
            data = SkWebpEncoder::Encode(pm, opt);
            break;
        }
    }
    if (!data) return {};
    const auto* p = static_cast<const std::uint8_t*>(data->data());
    return std::vector<std::uint8_t>(p, p + data->size());
}

std::uint64_t FrameEncoder::quickHash(SkSurface& surface) {
    SkPixmap pm;
    if (!surface.peekPixels(&pm)) return 0;

    std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    const int bpp = pm.info().bytesPerPixel();
    const int w = pm.width();
    for (int y = 0; y < pm.height(); y += 4) {
        const auto* row = static_cast<const std::uint8_t*>(pm.addr(0, y));
        if (!row) continue;
        // Step along the row too: 32-bit words every 8 pixels.
        for (int x = 0; x < w; x += 8) {
            const std::uint8_t* px = row + static_cast<std::ptrdiff_t>(x) * bpp;
            std::uint32_t v = 0;
            for (int i = 0; i < 4 && i < bpp; ++i) {
                v = (v << 8) | px[i];
            }
            h ^= v;
            h *= 1099511628211ull;  // FNV-1a prime
        }
    }
    return h;
}

}  // namespace folio::shell
