// folio/render/IPaintTarget.h
//
// The backend-neutral drawing vocabulary. Nothing in this file mentions Skia,
// Qt, OpenGL or any platform type.
//
// WHY THIS INTERFACE EXISTS
// -------------------------
// The toolkit decision (Qt Quick, with Slint as runner-up, architecture plan
// §2.2) is the single most expensive thing in the project to get wrong,
// because Qt needs a commercial licence for two of the six targets. Every
// screen, every control and every one of the 38 animations in the prototype
// paints through this interface, so changing backend recompiles one directory
// instead of rewriting the application.
//
// It is deliberately SMALL. Twenty-six primitives were extracted from the
// prototype's CSS; they all reduce to the handful below. Anything that cannot
// be expressed here - text shaping, accessibility, IME - is not a drawing
// problem and does not belong behind a drawing interface.
//
// ON RETAINED VERSUS IMMEDIATE
// ----------------------------
// This is an immediate-mode interface sitting on top of whatever retained
// scene graph the backend has. Qt Quick's QSGNode tree is retained; Skia's
// SkCanvas is immediate. The viewport (WP F1) keeps its own retained tile
// node either way, because that is the one place where rebuilding a scene
// graph per frame would actually cost something. Chrome is cheap enough to
// re-emit every frame and far simpler to reason about that way.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace folio::render {

struct Point { float x = 0.f, y = 0.f; };

struct Rect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;

    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr Rect inset(float d) const noexcept {
        return Rect{x + d, y + d, w - 2 * d, h - 2 * d};
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0.f || h <= 0.f; }
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/// Straight (non-premultiplied) RGBA, 0-255 per channel.
struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    [[nodiscard]] static constexpr Color rgb(std::uint32_t hex) noexcept {
        return Color{static_cast<std::uint8_t>((hex >> 16) & 0xFF),
                     static_cast<std::uint8_t>((hex >> 8) & 0xFF),
                     static_cast<std::uint8_t>(hex & 0xFF), 255};
    }
    [[nodiscard]] constexpr Color withAlpha(float a01) const noexcept {
        return Color{r, g, b, static_cast<std::uint8_t>(a01 * 255.f + 0.5f)};
    }
};

/// Per-corner radii. The prototype uses ten distinct radius tokens and two
/// controls with asymmetric corners, so a single scalar is not enough.
struct Corners {
    float tl = 0.f, tr = 0.f, br = 0.f, bl = 0.f;

    [[nodiscard]] static constexpr Corners all(float r) noexcept { return {r, r, r, r}; }
    [[nodiscard]] constexpr bool uniform() const noexcept {
        return tl == tr && tr == br && br == bl;
    }
};

/// One shadow layer. design-tokens.json composes elevations from up to two of
/// these plus a ring, e.g. elevation.card is
/// "0 6px 28px {shadow}, 0 0 0 1px {ring}".
struct Shadow {
    float dx = 0.f, dy = 0.f;
    float blur = 0.f;      // CSS blur radius; backends convert to their sigma
    float spread = 0.f;
    Color color{0, 0, 0, 40};

    /// True when the caller will immediately cover the shadow's own
    /// rectangle with an opaque fill, as every card and page does.
    ///
    /// A backend may then skip the interior entirely. This is not a
    /// micro-optimisation: the page shadow's interior is ~790k pixels of
    /// blended output that is overwritten on the next draw call, and on a
    /// CPU rasteriser that is several milliseconds per frame. The caller
    /// has to say so because only the caller knows whether what comes next
    /// is opaque.
    bool interiorOpaque = false;
};

enum class BlendMode : std::uint8_t { SrcOver, Multiply, Screen, Plus };

// --- paths -------------------------------------------------------------
//
// The prototype draws roughly sixty stroked icons. They cannot be composed
// from rectangles and circles (many are open curves with round caps), so the
// interface needs a path primitive. It is the minimum that can express an
// SVG icon: move/line/quad/cubic/close. Both candidate backends have a
// native equivalent - SkPath and QPainterPath - so this adds no emulation
// burden to either.
//
// Paths are *consumed*, not retained. Callers build them once at startup
// (see folio-ui IconRegistry) and hand the same span to every draw.

struct PathVerb {
    enum class Op : std::uint8_t { MoveTo, LineTo, QuadTo, CubicTo, Close };
    Op op = Op::MoveTo;
    /// Control points, in order. Unused slots are ignored.
    ///   MoveTo/LineTo : p0
    ///   QuadTo        : p0 = control, p1 = end
    ///   CubicTo       : p0, p1 = controls, p2 = end
    float x0 = 0.f, y0 = 0.f;
    float x1 = 0.f, y1 = 0.f;
    float x2 = 0.f, y2 = 0.f;
};

enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

// --- text --------------------------------------------------------------
//
// Text is the one part of the UI that cannot be expressed with the shape
// primitives above, and it is also the part with the largest gap between
// backends: Qt has QTextLayout, Skia has SkParagraph, and a naive port that
// positions glyphs itself will break the moment it meets Arabic, Devanagari
// or an emoji. So the interface deliberately exposes only *runs* - a string,
// a style, an origin - and leaves shaping entirely to the backend.
//
// Measurement lives here rather than on a separate interface because every
// backend measures with the same object it draws with (SkFont, QFontMetricsF),
// and splitting them guarantees that one day they disagree.

enum class FontWeight : std::uint16_t {
    Regular  = 400,
    Medium   = 500,
    SemiBold = 600,
    Bold     = 700,
};

/// Typeface families the UI can ask for. Deliberately three generic names
/// rather than real font names: the interface itself only ever uses Sans,
/// and the other two exist for document content the user chooses, where
/// the right answer differs per platform and per exported PDF.
enum class FontFamily : std::uint8_t { Sans, Serif, Mono };

struct TextStyle {
    float size = 13.f;
    FontWeight weight = FontWeight::Regular;
    /// Tracking in em, matching CSS `letter-spacing: 0.07em`. Multiplied by
    /// `size` at draw time.
    float letterSpacingEm = 0.f;
    Color color{0, 0, 0, 255};
    /// Last so that the existing positional initialisers - of which there
    /// are many - keep compiling untouched.
    FontFamily family = FontFamily::Sans;
};

/// Font-wide vertical metrics, in pixels at the style's size. `ascent` is
/// negative (above the baseline), following the convention of every font
/// format and both candidate backends.
struct FontMetrics {
    float ascent = 0.f;
    float descent = 0.f;
    float lineGap = 0.f;
    [[nodiscard]] constexpr float lineHeight() const noexcept {
        return -ascent + descent + lineGap;
    }
};

enum class TextAlign : std::uint8_t { Left, Centre, Right };

/// Where the y of a text origin sits. Baseline is what the backend natively
/// wants; the other two are what layout code actually has, and converting
/// between them needs font metrics, so the backend does it.
enum class TextBaseline : std::uint8_t { Alphabetic, Top, Middle };

class IPaintTarget {
public:
    virtual ~IPaintTarget() = default;

    // --- state -------------------------------------------------------------
    virtual void save() = 0;
    virtual void restore() = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotateDegrees(float deg) = 0;
    virtual void clipRect(const Rect& r, bool antiAlias = true) = 0;
    virtual void clipRoundedRect(const Rect& r, const Corners& c, bool antiAlias = true) = 0;

    /// Push a transparency layer. Needed for the prototype's cross-fades: a
    /// group of overlapping shapes faded as a unit, rather than each shape
    /// faded separately, which double-darkens the overlaps.
    virtual void saveLayerAlpha(float alpha) = 0;

    // --- primitives ---------------------------------------------------------
    virtual void clear(const Color& c) = 0;
    virtual void fillRect(const Rect& r, const Color& c) = 0;
    virtual void fillRoundedRect(const Rect& r, const Corners& radii, const Color& c) = 0;
    virtual void strokeRoundedRect(const Rect& r, const Corners& radii,
                                   const Color& c, float width) = 0;
    virtual void drawShadow(const Rect& r, const Corners& radii, const Shadow& s) = 0;
    virtual void fillCircle(const Point& centre, float radius, const Color& c) = 0;
    virtual void drawLine(const Point& a, const Point& b, const Color& c, float width) = 0;

    /// Vertical two-stop gradient. The prototype uses these for the page
    /// backdrop and the toolbar; a general gradient API can wait until
    /// something needs it.
    virtual void fillRectVerticalGradient(const Rect& r, const Color& top,
                                          const Color& bottom) = 0;

    virtual void setBlendMode(BlendMode m) = 0;

    // --- paths --------------------------------------------------------------

    /// Stroke a path. Coordinates are in the current transform's space, so
    /// icons are authored in a 24x24 box and scaled by the caller.
    virtual void strokePath(std::span<const PathVerb> verbs, const Color& c,
                            float width, LineCap cap = LineCap::Round,
                            LineJoin join = LineJoin::Round) = 0;
    virtual void fillPath(std::span<const PathVerb> verbs, const Color& c) = 0;

    // --- text ---------------------------------------------------------------

    /// Draw one shaped run of UTF-8. `origin.y` is interpreted per `baseline`.
    virtual void drawText(std::string_view utf8, const Point& origin,
                          const TextStyle& style,
                          TextBaseline baseline = TextBaseline::Alphabetic) = 0;

    /// Draw inside `box`, horizontally aligned and vertically centred, with
    /// an ellipsis if it does not fit. This is what almost every control in
    /// the prototype actually needs, and doing it once here keeps the
    /// truncation rule identical everywhere.
    virtual void drawTextInBox(std::string_view utf8, const Rect& box,
                               const TextStyle& style, TextAlign align) = 0;

    [[nodiscard]] virtual float measureText(std::string_view utf8,
                                            const TextStyle& style) = 0;
    [[nodiscard]] virtual FontMetrics fontMetrics(const TextStyle& style) = 0;

    // --- offscreen raster cache ---------------------------------------------

    /// Statistics for the backend's bitmap cache.
    struct RasterCacheStats {
        std::size_t entries = 0, bytes = 0, budget = 0;
        std::size_t hits = 0, misses = 0, evictions = 0;
    };

    /// Draw something expensive once, then blit it on every later frame.
    ///
    /// On a miss the backend allocates an offscreen bitmap `pxW` x `pxH`
    /// device pixels, calls `paint` with a target whose coordinate space is
    /// exactly that bitmap, keeps the result under `key`, and blits it into
    /// `dst`. On a hit only the blit happens.
    ///
    /// The caller - not the backend - chooses `pxW`/`pxH`, and that is the
    /// whole point. A page re-rasterised at every intermediate scale of a
    /// zoom animation would miss the cache on every frame; instead the
    /// caller rasterises at a quantised scale and lets this stretch the
    /// result, then re-rasterises once the gesture settles. Crisp when
    /// still, cheap while moving.
    ///
    /// Returns true if the content came from the cache.
    virtual bool drawCached(std::uint64_t key, const Rect& dst, int pxW, int pxH,
                            const std::function<void(IPaintTarget&)>& paint) = 0;

    /// Drop everything; called when the palette or page theme changes and
    /// every cached bitmap is stale.
    /// Blit a premultiplied BGRA8888 top-down buffer into `dst`, scaling if
    /// the sizes differ.
    ///
    /// This exists for PDFium, which hands back exactly this layout - the
    /// same byte order Skia calls kBGRA_8888_SkColorType, so no swizzle is
    /// needed on the way in.
    ///
    /// `cacheKey` is a promise, not a hint: pass non-zero only when the
    /// pixels at that key are immutable for as long as the key is live. The
    /// backend may then wrap them once and reuse that across frames, which
    /// is the difference between a per-frame multi-megabyte copy and a map
    /// lookup. Pass 0 for transient pixels - PDFium's own bitmaps, for
    /// instance, which it may reuse the moment the call returns. Call
    /// `invalidateImage` when a key's pixels go away.
    /// Whether these pixels are the final answer for this rectangle.
    ///
    /// Resampling quality is the single biggest cost in a page blit, so the
    /// caller has to say. `Transient` means "about to be replaced by a
    /// correctly sized raster" - a held bitmap mid-zoom - and buys the
    /// cheapest filter that does not look broken. `Final` means the user
    /// will sit and read this, so a genuine magnification is worth a cubic
    /// resample.
    enum class ImageFit { Final, Transient };

    /// Page-theme colour transform, applied to the image as it is drawn.
    ///
    /// This is the whole of the page-theme feature: the rasteriser always
    /// produces light-mode pixels, and the reading theme is a filter on the
    /// way to the screen. That keeps one cache entry per page per zoom
    /// instead of one per page per zoom per theme, and makes switching
    /// themes free rather than a full re-raster of everything on screen.
    ///
    ///   Sepia   multiplies by a warm paper colour, so coloured figures stay
    ///           coloured and only the whites warm up.
    ///   Invert  maps luminance onto an ink->paper ramp (a duotone). Real
    ///           inversion turns photographs into negatives; a duotone keeps
    ///           dark text light-on-dark without that.
    enum class ImageTint : std::uint8_t { None, Sepia, Invert };

    virtual void drawImageBGRA(const void* pixels, int width, int height,
                               std::size_t stride, const Rect& dst,
                               std::uint64_t cacheKey = 0,
                               ImageFit fit = ImageFit::Final,
                               ImageTint tint = ImageTint::None) = 0;

    /// Drop any cached upload held for `cacheKey`.
    virtual void invalidateImage(std::uint64_t /*cacheKey*/) {}

    virtual void invalidateRasterCache() = 0;
    [[nodiscard]] virtual RasterCacheStats rasterCacheStats() const = 0;

    // --- diagnostics --------------------------------------------------------
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
    [[nodiscard]] virtual std::string_view backendName() const = 0;
};

}  // namespace folio::render
