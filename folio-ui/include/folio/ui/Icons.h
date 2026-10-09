// folio-ui/include/folio/ui/Icons.h
//
// The icon set, authored as SVG path data and parsed once at startup.
//
// WHY SVG STRINGS RATHER THAN BAKED COMMAND ARRAYS
// ------------------------------------------------
// Icons change often and are produced by designers, not programmers. Keeping
// them in the exact `d` attribute that comes out of a drawing tool means an
// icon swap is a copy-paste, and it keeps this file diffable against the
// upstream icon library. The parse happens once per icon per process into a
// PathVerb array, so the per-frame cost is zero - we are not parsing strings
// in the paint loop.
//
// All icons are authored on a 24x24 grid with a 1.9 px stroke, matching
// design-tokens.json `icon.*`. The registry scales to the requested size.
//
// LICENCE NOTE
// ------------
// These paths are geometry in the style of Lucide (ISC licence, permissive,
// attribution in NOTICE). ISC permits redistribution in source and binary
// form with the copyright notice retained - no copyleft exposure. If the
// project later adopts a different icon set, the licence of that set must be
// re-checked; icon libraries are a common source of accidental CC-BY-SA or
// CC-BY-NC obligations.

#pragma once

#include "folio/render/IPaintTarget.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace folio::ui {

using folio::render::PathVerb;

/// Parse an SVG path `d` string into verbs.
///
/// Supports the full SVG path grammar except for the rarely used `R`/`B`
/// extensions: M m L l H h V v C c S s Q q T t A a Z z.
///
/// Elliptical arcs are converted to cubics via the endpoint-to-centre
/// parameterisation of SVG 1.1 F.6.5. They are not optional: 18 of the
/// icons below are drawn with `a` commands, and an earlier version of this
/// parser that bailed on unknown commands rendered them as a handful of
/// disconnected strokes - with no error, because a truncated path is still
/// a valid path.
///
/// Malformed input yields whatever was parsed up to the error rather than
/// throwing; a broken icon should not take down the app.
[[nodiscard]] std::vector<PathVerb> parseSvgPath(std::string_view d);

/// Append a circle approximated by four cubic segments. The magic constant
/// 0.5522847 is the standard circle-to-bezier ratio, 4/3 * (sqrt(2) - 1);
/// maximum radial error is about 0.02%, invisible at icon sizes.
void appendCircle(std::vector<PathVerb>& out, float cx, float cy, float r);

/// Named icon lookup with lazy parse and permanent cache.
class IconRegistry {
public:
    static IconRegistry& instance();

    /// Returns an empty span for an unknown name, so a typo renders nothing
    /// rather than crashing. Logs once per missing name.
    [[nodiscard]] std::span<const PathVerb> get(std::string_view name);

    /// Register or override an icon at runtime (used by tests).
    void define(std::string name, std::string_view svgPath);

    [[nodiscard]] std::size_t count() const noexcept { return cache_.size(); }
    /// Every name known to the registry, for the icon-sheet debug screen.
    [[nodiscard]] std::vector<std::string> names() const;

private:
    IconRegistry();
    std::unordered_map<std::string, std::vector<PathVerb>> cache_;
};

/// Draw a named icon centred in `box`, scaled from the 24x24 authoring grid.
///
/// `strokeScale` keeps the stroke visually constant: scaling a 1.9 px stroke
/// with the icon makes a 13 px icon look spindly and a 24 px one look fat,
/// so by default the stroke width is NOT scaled with the geometry.
void drawIcon(folio::render::IPaintTarget& t, std::string_view name,
              const folio::render::Rect& box, const folio::render::Color& colour,
              float strokeWidth = 1.9f);

}  // namespace folio::ui
