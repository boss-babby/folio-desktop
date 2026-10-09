// tests/ui/test_icons.cpp
//
// Regression guard for the icon path parser.
//
// WHY THIS EXISTS
// ---------------
// The parser originally had no case for the SVG arc commands 'A'/'a'. The
// failure mode was silent: an unrecognised command simply ended the path, so
// eighteen icons rendered as a few disconnected strokes. Nothing threw,
// nothing logged, the build was green and the tests passed. It was only
// caught by looking at a screenshot.
//
// So these checks assert the properties a human would have had to eyeball:
// every registered icon parses, produces a sane amount of geometry, starts
// with a MoveTo, stays inside its 24x24 viewBox, and contains no NaNs. The
// arc-specific cases below pin down the one construction that broke.

#include "folio/ui/Icons.h"
#include "../Harness.h"

#include <cmath>
#include <limits>

using namespace folio::test;
using folio::render::PathVerb;
using Op = PathVerb::Op;

namespace {

struct Bounds {
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    bool sawNan = false;

    void add(float x, float y) {
        if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) {
            sawNan = true;
            return;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
};

/// Count the points each verb actually uses, so a Close does not contribute
/// a spurious (0,0) to the bounds.
Bounds boundsOf(std::span<const PathVerb> path) {
    Bounds b;
    for (const PathVerb& v : path) {
        switch (v.op) {
            case Op::MoveTo:
            case Op::LineTo:
                b.add(v.x0, v.y0);
                break;
            case Op::QuadTo:
                b.add(v.x0, v.y0);
                b.add(v.x1, v.y1);
                break;
            case Op::CubicTo:
                b.add(v.x0, v.y0);
                b.add(v.x1, v.y1);
                b.add(v.x2, v.y2);
                break;
            case Op::Close:
                break;
        }
    }
    return b;
}

}  // namespace

FOLIO_TEST_MAIN("ui icons") {
    folio::ui::IconRegistry& reg = folio::ui::IconRegistry::instance();
    const std::vector<std::string> names = reg.names();

    section("every registered icon yields usable geometry");
    check(names.size() > 60, "registry is populated");

    int emptyCount = 0, nanCount = 0, outOfBox = 0, badStart = 0, tiny = 0;
    std::string firstEmpty, firstNan, firstOut, firstBadStart, firstTiny;

    for (const std::string& name : names) {
        const std::span<const PathVerb> path = reg.get(name);

        if (path.empty()) {
            if (firstEmpty.empty()) firstEmpty = name;
            ++emptyCount;
            continue;
        }

        // A path that begins with anything but a MoveTo has lost its head,
        // which is what a desynchronised argument scanner produces.
        if (path.front().op != Op::MoveTo) {
            if (firstBadStart.empty()) firstBadStart = name;
            ++badStart;
        }

        // At least one drawing verb. A path of nothing but MoveTos draws
        // no ink at all - which is what a parser that bails on an
        // unrecognised command tends to leave behind. Two verbs is a
        // legitimate icon ("minimise" is a single line), so the floor
        // cannot be set any higher than this.
        bool hasInk = false;
        for (const PathVerb& v : path)
            if (v.op != Op::MoveTo && v.op != Op::Close) hasInk = true;
        if (!hasInk) {
            if (firstTiny.empty()) firstTiny = name;
            ++tiny;
        }

        const Bounds b = boundsOf(path);
        if (b.sawNan) {
            if (firstNan.empty()) firstNan = name;
            ++nanCount;
            continue;
        }

        // Icons are authored in a 24x24 viewBox. A little slack absorbs
        // legitimate overshoot in control points; a parser that mangles
        // coordinates blows past this immediately.
        constexpr float kSlack = 4.f;
        if (b.minX < -kSlack || b.minY < -kSlack || b.maxX > 24.f + kSlack ||
            b.maxY > 24.f + kSlack) {
            if (firstOut.empty()) firstOut = name;
            ++outOfBox;
        }
    }

    check(emptyCount == 0, "no icon parses to an empty path (first: " +
                               (firstEmpty.empty() ? std::string("-") : firstEmpty) + ")");
    check(nanCount == 0, "no icon contains NaN or infinite coordinates (first: " +
                             (firstNan.empty() ? std::string("-") : firstNan) + ")");
    check(badStart == 0, "every icon starts with a MoveTo (first bad: " +
                             (firstBadStart.empty() ? std::string("-") : firstBadStart) + ")");
    check(tiny == 0, "every icon has at least one drawing verb (first bad: " +
                         (firstTiny.empty() ? std::string("-") : firstTiny) + ")");
    check(outOfBox == 0, "every icon stays near its 24x24 viewBox (first: " +
                             (firstOut.empty() ? std::string("-") : firstOut) + ")");
    note(std::to_string(names.size()) + " icons checked");

    section("the eighteen arc-using icons still produce curves");
    {
        // These are exactly the icons whose source path data contains an
        // 'A'/'a' command. Before arc support existed they parsed into a
        // handful of disconnected strokes and nobody noticed for a week.
        // An arc must become cubics, so each of these has to contain at
        // least one CubicTo.
        static const char* kArcIcons[] = {
            "folder", "folder-open", "file",  "file-text", "print",  "share",
            "rotate-cw", "rotate-ccw", "hand", "highlighter", "pen", "eraser",
            "stamp",  "link",  "moon",  "lock", "undo", "redo"};

        int missingCurves = 0;
        std::string firstFlat;
        for (const char* n : kArcIcons) {
            const auto path = reg.get(n);
            bool curved = false;
            for (const PathVerb& v : path)
                if (v.op == Op::CubicTo || v.op == Op::QuadTo) curved = true;
            if (!curved) {
                if (firstFlat.empty()) firstFlat = n;
                ++missingCurves;
            }
        }
        check(missingCurves == 0,
              "all 18 arc-using icons contain curves (first flat: " +
                  (firstFlat.empty() ? std::string("-") : firstFlat) + ")");
    }

    section("arcs convert to cubics");
    {
        // A half-circle written as a single arc. The endpoint parameterisation
        // has to be converted to centre form and split, because one cubic
        // cannot represent more than 90 degrees accurately.
        const auto p = folio::ui::parseSvgPath("M4 12a8 8 0 0116 0");
        check(!p.empty(), "arc path is not empty");
        check(p.size() >= 3, "semicircle splits into at least two cubics");
        bool allCubicAfterMove = true;
        for (std::size_t i = 1; i < p.size(); ++i)
            if (p[i].op != Op::CubicTo) allCubicAfterMove = false;
        check(allCubicAfterMove, "arc emits only cubics");

        const PathVerb& last = p.back();
        check(nearly(last.x2, 20.0, 0.05) && nearly(last.y2, 12.0, 0.05),
              "arc lands on its stated endpoint (20,12)");

        // The apex should sit a radius above the chord.
        const Bounds b = boundsOf(p);
        check(nearly(b.minY, 4.0, 0.2), "semicircle reaches y=4 at its apex");
    }

    section("compact flag syntax does not desynchronise");
    {
        // '0016' is four separate tokens: large-arc 0, sweep 0, x 1, y 6.
        // A generic number scanner reads "0016" as one number and every
        // following argument shifts. This is why flag() exists.
        const auto p = folio::ui::parseSvgPath("M2 2a2 2 0 0016 0");
        check(!p.empty(), "compact arc parses");
        const Bounds b = boundsOf(p);
        check(!b.sawNan, "compact arc has finite coordinates");
        check(b.maxX < 40.f, "flags were not swallowed into the x coordinate");
    }

    section("leading plus signs are accepted");
    {
        // std::from_chars rejects '+', but SVG path data permits it.
        const auto p = folio::ui::parseSvgPath("M+2 +2L+10 +10");
        check(p.size() == 2, "both verbs survived a leading '+'");
        if (p.size() == 2)
            check(nearly(p[1].x0, 10.0, 1e-4) && nearly(p[1].y0, 10.0, 1e-4),
                  "'+10 +10' parsed as (10,10)");
    }

    section("degenerate input is handled without crashing");
    {
        check(folio::ui::parseSvgPath("").empty(), "empty string yields empty path");
        check(folio::ui::parseSvgPath("   ").empty(), "whitespace yields empty path");
        // Truncated argument lists must not read past the end.
        (void)folio::ui::parseSvgPath("M4 12a8 8 0 01");
        (void)folio::ui::parseSvgPath("C1 2 3");
        check(true, "truncated paths do not crash");
    }

    // FOLIO_TEST_MAIN reports for us.
}
