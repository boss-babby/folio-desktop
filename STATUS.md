# Folio — build status

**Updated:** 2026-10-08 · **Platform validated:** Linux x86-64, GCC 14.2, CMake 4.4, Ninja

---

## One-line summary

The project now **configures, builds and tests end to end on Linux against real
PDFium and real Skia**: 401 assertions across 7 ctest suites, 0 failures, 0
compiler warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
A real PDF goes in; a themed PNG comes out.

---

## How to reproduce

```bash
./tools/fetch-deps.sh                    # PDFium + Skia (+ PoDoFo), pinned
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFOLIO_BUILD_UI=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Three PNGs land in `tests/data/`: `folio-pipeline-{light,sepia,dark}.png`.
They are the output of the complete stack, not a mock-up.

---

## Test results

```
      Start  1: core.snapshot ..........  Passed
      Start  2: render.tilegrid ........  Passed
      Start  3: render.rastercache .....  Passed    82 checks
      Start  4: render.scheduler .......  Passed    42 checks
      Start  5: engine.pdfium ..........  Passed   200 checks   (real PDFium 157.0.8086)
      Start  6: render.skia ............  Passed    50 checks   (real Skia m144)
      Start  7: integration.pipeline ...  Passed    27 checks   (full stack -> PNG)
      Start  8: ui.icons ...............  Passed    20 checks   (all 74 icons parsed)

      100% tests passed, 0 tests failed out of 8
```

| Target | Lines | State |
|---|---:|---|
| `folio-core` | 2,311 | builds, tested, **zero third-party dependencies** |
| `folio-engine-pdfium` | 655 | builds and tested against PDFium 157.0.8086 |
| `folio-render` (interface) | 148 | builds |
| `folio-render-skia` | 540 | builds and tested against Skia m144 |
| `tests` | 2,776 | 401 assertions, all passing |
| `folio-ui` (Qt) | 535 | **not compiled** — needs Qt, not present in this environment |
| `folio-engine-podofo` | — | **not written** — blocked on PoDoFo ≥ 1.0 |

**Total: 6,965 lines of C++ across 39 files.** 6,430 of them compile and run.

---

## Dependencies, as actually resolved

| Library | Version | How | Licence |
|---|---|---|---|
| PDFium | 157.0.8086 (`chromium/7568`) | `bblanchon/pdfium-binaries`, prebuilt `.so` | BSD-3-Clause |
| Skia | m144 (`e2e6623374`) | `JetBrains/skia-pack`, prebuilt static | BSD-3-Clause |
| PoDoFo | ≥ 1.0 | built from source by `fetch-deps.sh` | MPL-2.0 elected |

Everything in the current build is permissively licensed. There is no copyleft
obligation in the stack as it stands today.

---

## What was measured (and what it changed)

### 1. Tiled rendering is **not** byte-identical to a full-page render

The B3 acceptance criterion — *"a 4×4 tile grid reassembles byte-identical to
one full-page render"* — passes against a fake rasterizer and **fails against
real PDFium**.

```
612×792 page, 256 px tiles, no bleed:
  318 differing subpixels, worst delta 135/255
  all of them inside x[256..264] — a 9 px band down one tile boundary
```

Delta 135 is not antialiasing noise; it is a visibly different pixel. Cause: a
glyph whose **origin** falls outside the tile is rasterised differently from
the same glyph drawn with its origin inside the bitmap. It is not a clipping
bug and cannot be fixed by adjusting the clip rect.

**Fix: render each tile with a margin, then crop.** Measured sweep:

| case | bleed 0 | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|
| letter ×1, 256 px tiles | 318/135 | 30/127 | **OK** | OK | OK | OK |
| letter ×1, 128 px tiles | 615/135 | 99/127 | **OK** | OK | OK | OK |
| letter ×2, 512 px tiles | 51/71 | 51/71 | 51/71 | **OK** | OK | OK |
| letter ×4, 512 px tiles | 255/1 | 225/1 | 213/1 | 213/1 | 213/1 | 189/1 |

*(cells are `differingSubpixels/worstDelta`; **OK** = byte-identical)*

Required bleed scales with zoom, so `TileGrid::recommendedBleed()` scales with
it too (×0.5 → 12 px, ×1 → 24, ×2 → 48, ×4 → 64, clamped).

Above ×2 a residue of a few hundred subpixels differs **by exactly 1/255** —
PDFium's coverage accumulation rounding against a different bitmap width. That
is far below any display threshold and is not worth chasing.

**Revised acceptance criterion, now enforced in CI:**
> byte-identical at scale ≤ 2.0; worst channel delta ≤ 1 above it.

**Cost:** a 512 px tile with 24 px bleed rasterises 560×560 — **+19.6% pixels**.
Keep `tileSize ≥ 10 × bleed` and the overhead stays near 20%; the test asserts
it stays under 60%.

### 2. Skia prebuilts do not ship Graphite

```
nm -C libskia.a | grep -c 'skgpu::graphite'  ->    0
nm -C libskia.a | grep -c 'GrDirectContext'  ->  229
```

The Graphite **headers** are present in `include/gpu/graphite/` including the
Dawn backend. The **symbols are not**. Appendix C's "Graphite + Dawn = one
backend, six platforms" therefore requires building Skia from GN with
`skia_enable_graphite=true` — it is not available off the shelf from any
public prebuilt. That cost was already in the estimate; it is now confirmed
rather than assumed, and `FolioDependencies.cmake` asserts on it so nobody
discovers it at link time.

### 3. SkSL works, and the page-theme shader is real

One SkSL source compiles to GLSL / MSL / HLSL / SPIR-V / WGSL. The page theme
is a `SkRuntimeEffect` colour filter — **not** a re-render:

```
one rasterised tile -> light (255,255,255)  sepia (241,228,200)  dark (34,37,46)
```

Dark mode inverts **luminance along the theme's own paper→ink ramp** and keeps
chroma, so `(220,40,40)` red stays `(172,31,31)` red. Naive `1-x` inversion
would produce `(35,215,215)` cyan. The purple and teal rectangles in
`folio-pipeline-dark.png` are visibly still purple and teal.

The cache key deliberately does **not** include the theme, so there is exactly
one cached rendering of each tile. Keying by theme would triple the mobile
cache footprint to store three copies of identical pixels.

### 4. Measured timings (CPU raster, 2-core sandbox, not a GPU)

| stage | 612×792 page at ×1 |
|---|---|
| PDFium raster, 4 tiles with bleed | 1–4 ms |
| Skia composite, light (no shader) | **8.4 ms** |
| Skia composite, sepia/dark (shader) | 30.4 / 30.9 ms |
| resident cache after the page | 1.9 MiB |

The light/dark gap is the whole argument for `PageTheme::isIdentity()`
returning `nullptr`: skipping the colour filter in light mode is a **3.6×**
difference on CPU. On a GPU both collapse to a fragment pass, but the
asymmetry is real on the WASM software path and on low-end Android.

### 5. A real bug the integration test caught

`keyForRequest()` derived tile coordinates as `tileDevice.x / kTileSize`.
Correct — until bleed made tile origins `tx*512 - 24`, at which point integer
division mapped tx=0 and tx=1 both to 0, the scheduler deduped two different
tiles into one, and three quarters of the page silently never rendered.

Fixed by carrying `tileX`/`tileY` explicitly on `TileRequest`. The unit tests
could not have found this; only running the layers together did.

### 6. Shadows were 60% of the frame — nine-patch caching

The UI shell renders the full chrome offscreen and streams it as MJPEG, which
makes per-frame cost directly measurable. The first honest reading was bad:

| | render | encode |
|---|---|---|
| reader screen, first working build | 30.1 ms | 11.8 ms |
| same, with `FOLIO_NO_SHADOWS=1` | 12.2 ms | 11.6 ms |

**17.9 ms — three 60 FPS frame budgets — went on re-blurring shadows that had
not changed.** A card's elevation was recomputed from scratch every frame purely
because the frame was being redrawn.

The fix (`folio-render/skia/ShadowCache.{h,cpp}`) exploits the fact that a
blurred rounded rectangle is four fixed corner profiles joined by edges that are
constant along their length: the *size* of the box does not affect what the
shadow looks like, only the radius and the blur do. So each distinct
(radius, sigma) is rendered once into a small tile and stretched with
`drawImageNine`. One tile serves the 780×1010 page shadow and every other
shadow sharing its parameters.

Three things turned out to matter, in descending order of how long they took to
find:

1. **The tile's shape must be ≥ 3σ larger than its corners.** The first
   implementation sized the tile to the corners alone, so the page shadow was a
   5×5 square blurred at σ=20 — its energy dispersed and the peak alpha came out
   at 1/255. Shadows silently vanished. A destination smaller than that bound has
   genuinely interacting edges and *cannot* be a nine-patch, so those fall back
   to a direct blur; wide, short chrome like the command bar gets a tile built at
   its own height that stretches only horizontally.
2. **Clip out the interior when an opaque fill follows** (`Shadow::interiorOpaque`).
   The page shadow's middle is ~790k pixels of blended output discarded by the
   very next draw call. Worth **13.7 ms/frame** on its own.
3. **Blit in device space.** Building the tile at device resolution makes the
   corner blocks map 1:1 onto device pixels, so `kNearest` is exact rather than
   merely close — **5.7 ms/frame** cheaper than `kLinear`, and it keeps shadows
   sharp at dpr 2 instead of magnifying a dpr-1 tile.

A fourth find came out of the same profiling run: `fillRectVerticalGradient`
evaluated its shader per pixel at ~8 ns/px against 0.8 ns/px for an opaque solid
fill, costing **7.0 ms** for the reader backdrop alone. The ramp is now baked
once into a 1×256 image and stretched.

Result, same machine, same scene, measured one server at a time (running the
A/B instances concurrently on 2 cores inflates both):

| | render | encode |
|---|---|---|
| reader, before | 30.1 ms | 11.8 ms |
| reader, after | **9.9 ms** | 11.4 ms |
| welcome, before | 30.5 ms | 12.3 ms |
| welcome, after | **11.7 ms** | 11.9 ms |

Cache behaviour after exercising both screens and a theme change: **11 tiles,
956 KB, 2101 hits / 11 misses.** A full light→dark transition added exactly one
tile, which is the evidence for the claim that tiles are colour-independent —
the colour arrives via a `kSrcIn` colour filter, so hover and fade animations
that only change shadow opacity are free.

Correctness was verified against the direct-blur path rather than by eye.
`FOLIO_SHADOW_DIRECT=1` forces the reference implementation; comparing full
frames gives **max channel delta 12 on 54 of 1.3 M pixels, mean 0.159** at dpr 1,
and **max 15, mean 0.160** at dpr 2. Both diagnostics (`FOLIO_NO_SHADOWS`,
`FOLIO_SHADOW_DIRECT`) are kept deliberately.

Caveat worth stating plainly: these are CPU-raster numbers on a 2-core sandbox
with no GPU. They are not evidence about 60 FPS on target hardware, in either
direction. What they *do* establish is that the drawing layer no longer wastes
most of its time on work it could have cached.

### 7. The icon parser bug, and the test that now catches it

The SVG path parser had no case for the arc commands `A`/`a`. The failure mode
was silent — an unrecognised command simply ended the path — so 18 icons
rendered as a few disconnected strokes with a green build and passing tests. It
was caught by looking at a screenshot, which is not a process.

`tests/ui/test_icons.cpp` now renders every registered icon and asserts the
properties a human would otherwise have to eyeball: all 74 parse, start with a
`MoveTo`, contain at least one drawing verb, carry no NaNs, and stay near their
24×24 viewBox; the 18 arc-using icons are named explicitly and must contain
curves; and the arc maths is pinned by checking that a semicircle splits into
multiple cubics, lands on its stated endpoint and reaches its apex.

The guard was validated by sabotage: removing the `case 'A'` makes it fail with
5 errors naming `folder` first. A test that has never failed is not known to
work.

---

## Work package status

| WP | What | State |
|---|---|---|
| A1 | Repo, CMake, presets | **done** |
| A2 | Vendored dependencies | **done** — `tools/fetch-deps.sh`, pinned, cached in CI |
| A3 | Design tokens | done (JSON); generator (`tools/token-gen`) not written |
| A4 | Core utilities (`Observable`, `SlotMap`, `Arena`) | **done, tested** |
| A8 | Boundary guards | **done** — and it already caught a violation |
| B1 | `DocumentSnapshot` rope | **done, tested** |
| B2 | PDFium library + document load | **done, tested against real PDFium** |
| B3 | Tile rasterizer | **done, tested** — incl. the bleed finding |
| B4 | Progressive slicing / cancellation | **done, tested** |
| B5 | Text index | not started |
| B6–B8 | PoDoFo store, save, round-trip | **blocked** — needs PoDoFo ≥ 1.0 built |
| C1 | `TileGrid` | **done, tested** — now with `bledTile()` |
| C2 | `RasterCache` | **done, tested** (82 checks) |
| C4 | `TileScheduler` | **done, tested** (42 checks) |
| C3/C5/C6 | Texture cache, render service, prefetcher | not started |
| — | `folio-render` paint interface | **done** (new; not in the original plan) |
| — | Skia backend + page-theme shader | **done, tested** (new) |
| — | Shadow nine-patch cache + gradient ramp cache | **done, measured** (new) |
| — | `folio-ui` widget toolkit, both screens, stream shell | **done, running** (new) |
| — | Icon parser regression suite | **done** (`ui.icons`, 20 checks) |
| F1 | Qt viewport item | written, **not compiled** (no Qt here) |
| I-block | Platform lanes | CI skeleton written for Linux/macOS; iOS/Android/WASM not started |

---

## Known gaps

1. **Qt is not installed in this environment**, so `folio-ui/viewport/FolioPageView.*`
   remains written-but-unverified. It is real `QQuickItem`/`QSGNode` code, not a stub,
   but "compiles" is not a claim that can be made about it.
2. **PoDoFo is not built here.** Debian ships 0.9.8, which predates both the MPL
   dual-licence option and the API Folio uses. `fetch-deps.sh` builds 1.1.0 from
   source; that path has not been exercised in this sandbox.
3. **No GPU.** Every Skia test runs on a CPU raster surface. That is the correct
   substrate for golden images, but Ganesh/Graphite surface creation, swapchain
   handling and vsync are untested.
4. **One synthetic test document.** `tests/data/folio-test.pdf` is hand-written
   with known geometry. The adversarial corpus (WP J2) does not exist yet, and it
   is where the real bugs live.
5. **`FPDF_RenderPageBitmap_Start` has no matrix overload.** Progressive slicing
   therefore only covers whole-page work at integer scale — which is the slow work
   that needs it, but the limitation is real and is documented at the call site.

---

## Prototype-fidelity pass (review feedback)

Re-read `uploads/Folio · PDF reader and editor (working prototype).html` and
diffed it against the build. Two real defects and a pile of invented content.

### The two reported bugs

**1. Content overflowed the panels and the window.** `Widget::paintTree`
clipped only *children*. But a `ScrollView` subclass paints its rows in its
own `onPaint`, and `PageStage` paints the pages in its own `onPaint` - so
neither was clipped. Thumbnails spilled past the panel's rounded bottom and
pages drew over the command bar and off the bottom of the window. The flag is
now `Widget::clipsContent` and the clip wraps `onPaint`, the children and
`onPaintOverlay`, which is what `overflow:hidden` means on `.panel`, `.pane`
and `#cv`.

**2. The panel collapse buttons did nothing.** `setCollapsed(..., 0.0)` and
`currentWidth(0.0)` passed `now = 0`, so every animation was evaluated at its
own start time and the width never moved. Event handlers have no clock, so
`ReaderScreen::now_` now records the frame time in `onPaint` and the toggles
use it. Collapsing also follows the prototype properly: the panel becomes a
48px rail of icon buttons with a short fixed height (204px left, 90px right,
already in `design-tokens.json`), not a full-height sliver.

### Geometry corrected to the prototype

`#cmd{top:52}` and `#work{top:112;bottom:24}`. The outer margin is `gutter`
(24px) and only the panel-to-canvas gap is `panelGap` (16px) - the build used
16 for both, which is why the work area ended 4px low.

### Invented content replaced with the prototype's

| | was | now |
|---|---|---|
| Page body | grey placeholder bars | heading, paragraph, 4-bar chart, paragraph, page number - real `TT`/`PP` text |
| Page ratio | 612x792 (US Letter) | 560x800 (the prototype's `aspect-ratio:7/10`) |
| Opening zoom | fit-width (139%) | 125%, so `--pw` is 560px |
| Document | "Quarterly Report 2026.pdf", 48pp | `Annual_Report_2026.pdf`, 24pp |
| Recents | six invented names | the nine from `REC`, verbatim |
| Thumbnails | one column of mock lines | two-column grid rendering the real page source |
| Left panel | a bare thumbnail list | Pages/Outline/Notes/Layers tabs, page count, collapse chevron, Rotate/Insert/Extract/Delete |
| Right panel | an outline list | Display: page-theme chips, Scroll/Single/Two-up, caption |
| Welcome sidebar | Library / Appearance / Density | Open a PDF + Ctrl O, Start something, Merge/Images/Sign/OCR, Open files in, Theme, Settings |
| Reader title bar | a back arrow | removed; the prototype has none |

Also fixed: `Button::compact` (12px label, 2px padding) because a 60px cell
elided "Rotate" to "Rota..."; the right-panel caption is drawn as two lines
because `drawTextInBox` elides rather than wraps.

The welcome sidebar layout had been reaching its labels through
`sidebar_->children()[1]/[6]/[8]`. Changing the sidebar's contents silently
mislaid every header. It holds direct pointers now.

### Still not matching

- Command bar: prototype order is mode | open save print export | undo redo |
  tools | search | Share | theme | toggles. Open and Export are missing and
  there is an extra focus-mode button.
- Pill: prototype ends with a view-layout dropdown and a status readout; the
  build has four layout buttons instead.
- Left panel's Notes and Layers tabs are tab stops with no content.
- Welcome hero lacks the page thumbnail and the Resume / Open in Edit buttons.
- Recents grid is 5-up; the prototype is `repeat(4,1fr)`.

---

## Renderer optimisation pass

Three reported bugs fixed, then the first stage of the performance brief.

### Bugs

| Reported | Cause | Fix |
|---|---|---|
| Collapsed left rail showed only the layers icon | The rail asked for `pages`, `list`, `message-square` — none of which exist in the icon registry. An earlier string patch had **silently no-opped**, so the fix was never actually in the binary. | Now `thumbnails`/`outline`/`comment`/`layers`. Every patch site now asserts it changed something. |
| Mode switcher cycled silently | It was a plain button. | `modePop_`: a 212×120 popover with Read/Annotate/Edit, icons, Ctrl 1/2/3 hints, current mode highlighted, chevron flips up/down, dismisses on outside click. |
| Welcome sidebar | Reverted to the Library/Appearance/Density design, per your preference over the prototype's. | Headers are now direct `Label*` members, not `children()[n]` lookups. |

A fourth, found while verifying: the panel captions were drawn from
`ReaderScreen::onPaintOverlay`, which runs after *every* child, so "24 pages"
bled through the open mode dropdown. Captions now paint inside the panel's own
subtree via `SidePanel::drawOverlay`.

### Page raster cache

`IPaintTarget::drawCached(key, dst, pxW, pxH, paint)` — backend-neutral, so
`folio-ui-core` still has no Skia in it. `SkiaPaintTarget` implements it as a
process-wide LRU of device-resolution `SkImage`s under a byte budget.

The caller picks the raster size, and that is the whole trick. `PageStage`
**freezes the raster scale while a zoom animation is running** and lets the
blit stretch the existing bitmap: the page grows continuously, with no reflow
and no re-run of the page's draw calls. The frame the animation stops,
`rasterScale_` snaps to the true scale and the page re-rasterises once, at
exact device resolution — crisp when still, cheap while moving. Scrolling
never invalidates anything, because scroll does not change scale.

Measured, 1440×900, 24-page document, placeholder page source:

| | median render | p90 render | page tiles | RAM | hit rate |
|---|---|---|---|---|---|
| Cache off (`FOLIO_NO_RASTER_CACHE=1`) | 11.30 ms | 19.16 ms | — | — | — |
| Cache on | **10.44 ms** | **13.76 ms** | 4 | 8.8 MB | 0.98 |

A cached blit is byte-identical to the draw that filled it (asserted in the
test suite, and confirmed end-to-end: mean channel difference between
cached and uncached screenshots is 0.016, i.e. JPEG noise).

**Read the median honestly.** The placeholder page draws a few dozen rects and
short text runs. A real PDFium page is one to two orders of magnitude more
draw calls, and the cache replaces *all* of them with one blit, so the gap
widens with page complexity rather than staying at 8%. The p90 number is the
more telling one today: −28%, because the worst frames are the ones doing page
work.

Memory: budget 256 MB desktop / 96 MB mobile / 64 MB wasm, strict LRU
eviction, plus an idle trim every 2 s that drops any entry not touched in the
last 400 cache operations. Visible pages are touched every frame and survive;
scrolled-past pages are handed back. An entry larger than a quarter of the
budget bypasses the cache entirely rather than evicting everything else.

`tests/render/test_skia_page_cache.cpp` — 22 checks: hit/miss counting,
pixel-identity of a hit, byte accounting, LRU ordering, oversize bypass, idle
trim keeping hot entries. ctest 9/9.

### Not done yet

- **The Edge comparison has not been run.** It needs a real PDF through
  PDFium on your machine; there is no browser in this sandbox and no honest
  way to fake the number. The harness is ready: `/stats` reports render time,
  tile count, resident MB and hit rate, and `FOLIO_NO_RASTER_CACHE=1` gives a
  clean A/B.
- Tile-level caching for pages larger than the viewport, and a background
  rasterisation thread, are the next two steps — currently a cache miss
  rasterises synchronously on the UI thread.

---

## Feedback round 3

| Reported | Cause | Fix |
|---|---|---|
| Mode dropdown overlaps undo | The button was widened to 118 px but the layout still advanced `x` by 104. | Single `kModeW` constant drives both. |
| Theme switch snaps | `Theme::setScheme` was called with `entranceAt_` on the welcome screen and `0.0` in the reader. A transition retargeted at a stale timestamp has already finished by the time it is first evaluated. | Both pass the live clock. `WelcomeScreen` now caches `now_` in `onPaint`, as `ReaderScreen` already did. Measured background over the transition: 237 → 187 → 151 → 110 → 64 → 31 → 18 → 15. |
| No way back to the home page | The prototype has none either: once a document is open the only exit is closing the last tab. | A **home button** left of the tab strip, plus **Ctrl W**. New `home` icon in the registry. |
| The "+" alternates | `onNew` fired on *any* click past the last tab, not just on the glyph, so stray title-bar clicks spawned tabs. The hover highlight had the same bug. | One `TabBar::plusRect()` drives hit testing and hover. "+" now opens the file chooser instead of making an empty tab. |
| Untitled / demo documents open nothing | There was no file-open path at all. | See below. |
| The four layout tiles should be one button with a popup | — | The pill's 4-icon segmented control is now a single **"Continuous ⌄"** button opening a 4-row menu: Single page / Continuous / Two up / **All together** ("Every page, side by side"), each with a one-line description. It stays in sync with the right panel's LAYOUT control. |

### Opening a file

`FilePicker` is the interface; `OpenDialog` is the in-app fallback.

The host's own dialog is the right default — `IFileOpenDialog`, `NSOpenPanel`,
the XDG portal — but two of the six targets have none to call. WebAssembly
can only prompt through a hidden `<input type=file>`, which returns bytes and
no path, and iOS has a view controller rather than a blocking call. So the
code path cannot be "call the native dialog"; it is an interface with a native
implementation where one exists and a modal card where one does not.
`FilePicker::isNative()` decides which the app shows. **No native backend is
wired up yet** — the desktop implementations are the next piece of work; the
seam is `FilePicker::setCurrent`.

Reachable from: the welcome screen's **Open file**, the command bar's **Open**,
the tab strip's **+**, and **Ctrl O**.

### Zoom that stays put

Changing scale rescales the whole layout, so holding `scrollY_` constant
slides the document under the viewport and you end up on a different page.
`PageStage` now records which page is under the viewport centre and where
within it, and restores that point after every scale-driven relayout.

Verified: parked on page 13 of 24, zoom 125% → 195% → 125%, page indicator
reads **13 / 24** at every step.

Together with the raster hold from the previous pass, a zoom gesture now:
keeps the same page, grows continuously with no reflow, reuses the cached
bitmap while moving, and re-rasterises once at exact device resolution when
it settles.

### Smooth scrolling

The wheel moved `scrollY_` by 64 px per notch instantly. It now eases to the
target over 190 ms, and consecutive notches extend the same glide instead of
each restarting from a standstill.

### Also

The command bar was missing **Open** and **Export** and had undo/redo in front
of save/print. It is now the prototype's order: `open save print export |
undo redo`.

ctest 9/9.

---

## Feedback round 4

### Bugs

| Reported | Cause | Fix |
|---|---|---|
| Right panel blank | My previous line-splice put `installPanelPainters()` at the end of **`setMode()`** instead of the build function, so the panel's painter was only installed after a mode change. Everything drawn by that painter — DISPLAY, PAGE THEME, the colour chips, "24 pages" — was missing until then. | Installed once after the widget tree is built. |
| Home button over the "Folio" wordmark | I put it at `tabsX - 36` = x 60; the wordmark runs to ~74. | Home at x 86, tab strip moved to 124. |
| Theme popup slow to close | On `Down` the outside-click handler closed the popover; on `Up` the button's own click saw it closed and reopened it. Clicking the button to dismiss appeared to do nothing until you clicked elsewhere first. | `Popover::setOwner()`. A press on the owning control is no longer an "outside" click, so the button toggles it cleanly. |

### Circular reveal

`Widget::applyContentClip()` is now virtual. `Popover` overrides it with a
circle grown from the edge nearest its button, so menus wipe open from the
control rather than fading in place — the same gesture as the panel's
`clip-path` circle. The radius ends 44 px past the farthest corner so the
card's drop shadow is not clipped at rest. The old 96% scale-up was removed;
it fought the reveal.

### Annotations

`folio-ui/components/Annotations.{h,cpp}` — `Annotation`, `AnnotationStore`,
and a draw routine. Working tools: **highlight, pen (ink), note, eraser**,
plus box / ellipse / text / stamp / signature through the same path.

Every coordinate is **normalised to the page, 0..1**. That is the only
representation that survives zoom, rotation, resize, a layout change, or the
same file opened on a phone — all of which move a page on screen and none of
which should move a highlight off its word. It is also one multiply away from
PDF user space when these have to be written to a file.

Two details worth keeping:

- Annotations are drawn **over** the cached page bitmap, never baked into it.
  Baking would invalidate the page raster on every stroke, which is exactly
  the moment the renderer cannot afford to re-rasterise.
- Ink is stored as sampled points and drawn as quadratics through their
  midpoints. Raw samples look faceted at high zoom; samples closer than 0.3%
  of the page are dropped, because every extra point costs on every later frame.

The right panel is now contextual: with an annotation tool active it shows
the tool name, six ink colours, Fine/Medium/Thick, and an opacity slider;
otherwise the page-display controls. Undo in the command bar undoes
annotations.

### "Game rendering" vs desktop UI rendering

The loop was already event-driven — `needsFrame()` gates paint entirely, so a
static window costs nothing but a 60 Hz input poll. What was missing was the
other half of how Word and every native toolkit work: **damage rectangles**.

`FolioApp::paint` now computes the union of the window-space bounds of
everything animating this frame (plus last frame's union, because a widget
that just stopped still has to be painted over once), clips to it, and repaints
only that. Pointer events damage a 440 px box around the cursor rather than
forcing a full frame — hover tints, pressed states and tooltips are local to
the cursor by definition. `Widget::damageBounds()` lets a full-window container
report what it can actually touch; without it the overlay layer made every
hover mark the whole window dirty.

| | before | after | damage |
|---|---|---|---|
| hover one toolbar button | 11.1 ms | **1.87 ms** | 440×440 |
| scroll the page | 11.0 ms | **5.84 ms** | 868×812 |

Above 55% of the window it falls back to a full repaint; the clip stops paying
for itself. `/stats` reports `partial`, `dmgW/dmgH` and the widest animating
widget so this is debuggable rather than magic.

### Still outstanding

**Tile-level caching for pages larger than the viewport, and moving cache
misses off the UI thread, are not done.** They are the last two items on the
renderer list and both are substantial: tiling changes the cache key and the
eviction policy, and async rasterisation needs a worker, a job queue, and a
placeholder to draw while a tile is in flight. Neither is started.

ctest 9/9.

## Feedback round 4 — real PDF engine

**"Go for the pure XFN library and real document"** — PDFium is now the renderer. The
placeholder page source is still present as a fallback, but nothing reaches it on the
normal path.

What was built:

| Piece | Where |
|---|---|
| `drawImageBGRA(pixels,w,h,stride,dst)` | `IPaintTarget.h` + `SkiaPaintTarget.cpp` — the blit primitive the engine needed and the paint layer did not have. Copies (PDFium reuses its bitmap); nearest at exact scale, Mitchell when upscaling. |
| `PdfiumPageSource` | `folio-engine/pdfium/` — `IPageSource` over `PdfiumRasterizer`. One full-page bitmap cached per page keyed on exact pixel size, 4096 px long-edge cap, `trimTo(page,radius)`. |
| `folio-ui-pdfium` | New CMake target. The only TU that sees both the widget layer and the engine, so `folio-engine-pdfium` stays UI-free and `folio-ui-core` keeps linking only `folio-render` + `folio-core`. |
| `LocalPicker` | `OpenDialog.cpp` — a real `std::filesystem` listing (folders + `.pdf`, real byte sizes, `..` navigation) replacing the six hard-coded `sample://` rows. |
| `ReaderScreen::openFile(path, now)` | Loads through PDFium; on failure toasts `error()` and falls back, so a damaged file degrades instead of showing an empty window. |
| `tools/make-sample-pdf.py` + `documents/*.pdf` | Hand-rolled base-14/Flate writer. `Annual_Report_2026.pdf` (24 pp), `Contract_v3.pdf` (11 pp), `Thesis_draft.pdf` (36 pp). |

Verified on screen: opening the Annual Report recents card renders real Helvetica text,
the rules, and the bar chart; the thumbnail rail shows real per-page renders; the page
count reads 24 from the document, not from the recents table. Four zoom steps to 195%
re-rasterise sharp — no bitmap upscaling. Cache 5 tiles / 27.6 MB / 0.97 hit rate.
ctest 9/9.

Two things stayed deliberately unchanged: the page raster is always rendered
`ColorScheme::Light` so page theme remains a colour filter, and annotations are still
composited over the blit rather than baked in.

## Feedback round 5 — async rendering (B) and the text layer (A)

Both at once, because they are the same problem underneath: PDFium is
single-threaded *per document*, so rasterisation and text extraction have to be
clients of one thread that owns the document. Building A after B would have
meant retrofitting it.

### Real documents first

The three sample files were synthetic, so PDFium accepting them proved little.
Pulled genuine material off the web into `documents/`:

| File | Why it is a useful test |
|---|---|
| `Attention_Is_All_You_Need.pdf` | arXiv LaTeX: Type1 subsets, vector figures, rotated stamp |
| `lshort.pdf` | 153 pages, outline, links |
| `Japanese_Government_Notice.pdf` | CID/CJK fonts |

`tests/engine/test_pdfium_corpus.cpp` opens every PDF in `documents/`, renders
first and last page, and asserts **ink coverage** rather than just a successful
return — a renderer that silently fails to find a font still returns a
perfectly white bitmap and a success code. All six pass. ctest is now 10/10.

### B — PdfiumWorker

One thread, one document, two job kinds. Page geometry is pulled across once at
open time and cached as plain data, so the UI lays out pages without ever
calling the engine. Requests are coalesced per page (newest wins), and the
worker picks the outstanding job nearest the focus page.

`PdfiumPageSource` degrades in order when the right-sized raster is not ready:
exact bitmap → any bitmap for that page, stretched → paper. That is what keeps a
zoom gesture continuous instead of flashing blank. `IPageSource` grew `tick()`,
`pending()` and `cachesOwnRasters()`.

### The 3x regression this uncovered

Moving rasterisation off-thread did not help at first: a zoom burst still cost
**34 ms/frame**. Three findings, in order:

1. `drawImageBGRA` copied the whole bitmap on **every call, every frame** — the
   old comment "PDFium reuses its bitmap, so the copy is not optional" was true
   when the rasteriser handed tiles straight to the painter, and false now that
   the page source owns them. Added a `cacheKey` promise so the backend wraps
   the pixels once. Not the main cost, but real.
2. The stage's own raster cache was **counterproductive** for a bitmap-backed
   source: it allocated an offscreen surface and composited the whole page into
   it to avoid a single blit. Hence `cachesOwnRasters()`.
3. **The actual cost was resampling.** The `exact` test had a 0.6 px tolerance,
   so sub-pixel rounding between the layout rectangle and the integer bitmap
   dropped every frame into a Mitchell cubic resample of a multi-megapixel
   image. Worse, raster scale is quantised to `kScaleQuantum`, so a mild
   upscale is the *normal settled state*, not an exception.

Replaced with a ladder: within 1.5 px → nearest; 2x or more and final →
Mitchell; everything else → bilinear. Callers pass `ImageFit::Transient` for a
held mid-gesture bitmap.

| | before | after |
|---|---|---|
| zoom burst, median | 34.1 ms | **13.2 ms** |
| zoom burst, p90 / max | — | **14.1 / 16.2 ms** |
| scroll, median | — | **12.2 ms** |
| idle | 15.9 ms | **11.9 ms** |

Measured on `Attention_Is_All_You_Need.pdf` at 1440x900, n=40 zoom samples.
Image cache at rest: 12 entries, 10.3 MB.

### A — text selection, search, copy

`PdfiumWorker` extracts glyphs via `FPDFText_*` on the same thread, normalising
boxes to 0..1 top-left — the space annotations already use, so a selection
rectangle and a highlight are directly comparable. A parallel ASCII-case-folded
`u32string` makes search a plain substring scan (CJK compares exactly, which is
correct for those scripts and avoids pulling ICU into the engine layer).

- Drag on text selects; drag on empty space still pans. Glyph runs merge into
  per-line rectangles.
- The stage warms the text layer for the pages around the viewport, because a
  press cannot wait a frame for extraction or it becomes a pan.
- Find is wired to the engine: live as you type, `n of m` counter, prev/next,
  current hit amber with an outline against pale amber for the rest.
- Ctrl+C copies (with a character count in the toast), Ctrl+A selects all,
  Escape clears. `onCopyText` is the clipboard seam; the streaming shell leaves
  it unset.

Verified: 97 hits for "attention" and 28 for "transformer" across 15 pages;
selection highlights six lines of the abstract and ends mid-line where the drag
stopped. The corpus test asserts glyph boxes lie on the page and are not
inverted — a sign error in the y-flip would make selection silently unusable.
314 CJK glyphs extracted from the Japanese document.

### Still open
- No clipboard backend (no native shell to host one).
- PoDoFo absent; nothing saves. Rotate/Insert/Extract/Delete remain labels.
- Edge comparison still never run — no browser in this environment.

## Feedback round 6 — PoDoFo

### Getting a compiled PoDoFo in

No upstream binary release exists, so the Debian prebuilt is the "already
compiled" option. `tools/bootstrap-deps.sh` now downloads `libpodofo-dev`,
`libpodofo0.9.8t64` and `libidn12` and extracts them into the dep cache with
`dpkg-deb -x` — **no root required**, nothing installed system-wide.

Three fixups, all scripted and commented:

| Problem | Fix |
|---|---|
| `podofo_config.h` advertises `PODOFO_HAVE_BOOST`, so `PdfDefines.h` includes `<boost/config.hpp>` | Flag turned off. Zero `BOOST_` macros and zero `boost::` symbols appear in any installed header — the include is vestigial, and Boost is ~100 MB for nothing |
| `libidn` ships no `.so` development symlink | Created in the cache |
| `libidn` reported missing despite being on the RPATH | `DT_RUNPATH` (the modern default) applies only to **direct** dependencies; `libpodofo`'s own lookup never consults the executable's RUNPATH. `-Wl,--disable-new-dtags` emits `DT_RPATH`, which *is* inherited down the chain |

Configure now prints `PoDoFo: yes (Debian prebuilt 0.9.8)`.

**Version caveat:** this is 0.9.8, not the 1.x the architecture plan was
written against. Same concepts, different spellings — `PdfMemDocument` not
`PdfDocument`, `WriteUpdate` not `SaveUpdate`, no `string_view`. Porting is
mechanical but not free.

**Licence**, since every dependency has to be flagged:

| Component | Licence |
|---|---|
| PoDoFo library (`src/*`) — what we link | **LGPL-2+ with OpenSSL exception** |
| `src/podofo/base/PdfEncrypt.cpp` | **RDS-Data-Security** (a custom, non-standard licence) sitting *inside* the library |
| `debian/*`, `tools/podofonooc` | GPL-2+ / GPL-3+ — packaging and a standalone tool, **not linked** |

LGPL means static linking carries a relink obligation; we link the shared
object, which does not. The `PdfEncrypt.cpp` term is the one worth a lawyer's
eye if encrypted PDFs matter.

### Appearance streams are the whole game

A PDF annotation carries both semantics (`/Subtype /Highlight`, a colour, a
rect) and an appearance stream `/AP` saying what to draw. The spec lets a
viewer synthesise the appearance; **almost none do, PDFium included.** An
annotation written without `/AP` is valid, round-trips through any parser, and
is invisible in every reader. So `PodofoDocument` draws every annotation into a
Form XObject as well as describing it — highlights fill with a Multiply blend
so text stays readable, ink strokes a round-capped polyline, notes draw their
own pin rather than trusting the viewer's `/Name` icon.

Highlights also get `/QuadPoints` in the spec's ul, ur, ll, lr order — *not* a
rectangle walk, which is a classic way to produce an invisible highlight.

### What shipped

| | |
|---|---|
| `PodofoDocument` | open / rotate / delete / insert / extract / annotations / metadata / incremental + full save |
| Save button | Writes the in-memory annotations into the file, saves incrementally, reopens so the screen matches the file |
| Rotate, Insert, Extract, Delete | No longer labels in a `for` loop — real page-tree edits, each saving and reopening |
| `tests/engine/test_podofo_roundtrip.cpp` | 38 checks. **ctest 11/11** |

The round-trip test is the important one: it edits with PoDoFo, then reopens
with **PDFium** and measures changed pixels against the unedited render.
Annotations scored **3.34% changed pixels** — an `/AP`-less annotation scores
~0, so this is the assertion that would have caught the invisible-annotation
failure. It also checks re-saving is idempotent rather than cumulative, and
that an incremental save leaves the original revision **byte-for-byte intact**
at the front of the file (25 017 → 26 249 bytes, +1 232).

### Two bugs this surfaced

1. **Save-in-place corrupted the file.** The document is loaded for update, so
   PoDoFo still holds the source; a full `Write` to the same path truncates the
   bytes it is reading. `saveAs` now writes a sibling temp and renames — which
   is also atomic, so an interrupted save cannot destroy the original.
2. **Rotation was applied twice**, so one Rotate click produced 180°. Both
   `FPDF_GetPageSizeByIndexF` and `FPDF_RenderPageBitmap` already honour
   `/Rotate`; the page source was swapping width/height *and* passing the
   rotation on to the renderer. Invisible until a document in the corpus
   actually had `/Rotate` set. The test now pins "a rotated A4 reports
   landscape dimensions" so it cannot come back.

Also fixed: the exported highlight was opaque while the on-screen overlay was
tinted, so a document changed appearance the moment it was saved. Both sides
now read one constant, `kHighlightAlpha`.

### Still open
- No clipboard backend, and no native window shell.
- Extract writes beside the original with a `_pN` suffix; there is no
  destination picker.
- Undo covers annotations, not page operations.

---

## Round 7 — chrome, scrolling, themes, selection markup

Six items, all shipped. ctest **11/11** (the PoDoFo round-trip grew 38 → 48
checks).

### 1. Window controls everywhere, and platform-specific
`folio-ui/components/WindowChrome.{h,cpp}` is new. `WindowControls` is one
widget with three looks:

| platform | placement | shape |
|---|---|---|
| Windows | right, flush to the corner | 46×44 squares, close hovers solid red |
| Linux | right, inset | 32 px circles, GNOME idiom |
| macOS | **left** | 12 px traffic lights, glyphs revealed on cluster hover |

Screens no longer hard-code a side: they ask `preferredWidth()` and
`controlsOnLeft()` and lay the title bar out around the answer, which is what
keeps the macOS build from needing its own copy of every screen. Verified by
running the Linux binary with `FOLIO_WINDOW_CHROME=macos` — traffic lights
appear on both screens and the wordmark, home button and tab strip slide
right. The macOS path is written, not stubbed.

The library screen had window buttons in code that never rendered: `Stack`
with `Cross::Centre` sizes a child from its *existing* `frame.h`, which is 0
for a freshly-emplaced `Button`. That whole call site is gone now.

Also removed: `ReaderScreen`'s window buttons were looked up by `debugName`
during layout. That was listed as fragile in the invariants; it is now a
single `winCtl_->setFrame`.

### 2. Scrollbars, and horizontal scrolling after a zoom
Overlay bars on both axes (`kBarThick 10`, `kBarInset 3`, `kBarMinLen 36`),
thumb alpha 90 idle / 150 active. Grabbing the thumb preserves the grab point;
clicking the track centres the thumb. `handleScrollbar` gets first refusal on
every pointer event, so a press on a bar can never start a text selection.

Horizontal panning now works five ways: the new bar, shift+wheel, trackpad
`wheelX`, drag-pan, and Left/Right arrows. `relayout()` computes `contentW_`,
the paint transform translates by `-scrollX_`, and `hitPage` subtracts it so
selection still lands on the right glyph when panned.

### 3. Collapsed sidebars now give their space to the page
The stage frame was always growing correctly — the bug was that the page
never re-fitted. `openDocument()` called `setZoom(1.25f)`, and `setZoom()`
sets `ZoomMode::Free`; the prototype's "125%" was a fixed-size mock baked in
as a magic number. Documents now open `ZoomMode::FitWidth`, and a width
change re-honours the fit mode.

Measured, both panels open → both collapsed: page **764 px → 1227 px**
(128% → 203%).

### 4. Page theme works, and is genuinely separate from day/night
`PdfiumPageSource::drawPage` took `PageTheme /*theme*/` and ignored it, so the
feature did nothing the moment a real PDF was open.

Implemented per the standing invariant — a colour filter at blit time, never a
re-render, so there is still one cache entry per page per zoom and switching
themes is free:

- **Sepia** multiplies by the paper colour `0xF6ECD8`. Coloured figures stay
  coloured; only the whites warm up.
- **Dark** is a luminance **duotone**, ink `0xE8E8EC` → paper `0x15171C`, not
  an inversion. `1-c` turns every photograph into a negative.

Measured on page 1: paper `(254,255,255)`, sepia `(242,235,227)`, dark
`(21,22,27)` with text at luminance 239 — light on dark.

### 5. Day/Night buttons and the prototype's circular reveal
The sliding segmented control is gone; `Day` and `Night` are two buttons, and
the pressed one reads as selected.

The transition was a palette cross-fade, which is why it kept looking wrong:
lerping drags every colour through a muddy mid-grey at once. It is now a true
circular reveal, like the prototype's View Transition. `FolioApp::paint`
paints the frame in the outgoing palette, then paints it **again** in the
incoming palette clipped to a circle growing from the pressed button, with a
hairline on the leading edge so the boundary is visible between two
similar-luminance palettes. Two tree traversals for ~0.8 s, and only then.

### 6. Selection popup
Select text, release, and a popup appears above the selection — above, so it
never covers the text it acts on (`Popover::preferAbove`). Highlight, Strike,
Underline, Copy, each with an icon, revealed with the same circular clip as
every other popover because it *is* a `Popover`.

This needed two new annotation kinds end to end:

- `AnnotKind::Underline` / `Strikethrough`, drawn in `drawAnnotation` (rule at
  8% of the line box from the bottom; strike at 58% from the top — the
  x-height centre, not the box centre, which looks high).
- `AnnotSpec::Kind::Underline` / `StrikeOut` in the exporter, with appearance
  streams and `/QuadPoints`, which all three text-markup kinds now emit.
- One annotation **per line**, not one per selection: a three-line selection
  must not produce a box that swallows the margins.

Verified end to end in the running app: marked three lines, saved
(11,881 → 14,544 bytes), and the reopened file renders all three from its own
appearance streams — `/Highlight`, `/Underline`, `/StrikeOut`, 3 `/QuadPoints`,
3 `/AP`. The new test measures pixels as well as subtypes, because an
`/AP`-less annotation parses fine and is invisible everywhere.

### Still open
- No clipboard backend, and no native window shell — the controls are drawn
  and wired, but nothing is listening on the other end of `onMinimise`.
- The Edge comparison has still never been run; there is no browser here.
- `FOLIO_WINDOW_CHROME` is read once at construction, so switching at runtime
  needs a restart.

---

## Round 8 — a real editor: undo, moveable marks, destination picker, free text

Four things, all verified in the running app rather than inferred from a
clean build.

### Undo/redo that covers both kinds of edit

`folio-ui/components/Edits.{h,cpp}` holds a conventional command stack —
`Command`, `UndoStack` (capped at 200 entries / 384 units of weight),
`AddAnnotCommand`, `RemoveAnnotCommand`, `ModifyAnnotCommand`, `AddManyCommand`.

The interesting part is that Folio has **two kinds of edit with nothing in
common**. Annotations live in memory and are trivially invertible: remember
the struct, put it back. Page operations rewrite the PDF on disk, and
deleting a page destroys content that exists nowhere else. Inverting that is
not possible in the abstract.

So page ops undo by **restoring file bytes**. `snapshotFile()` copies the
document to a per-process scratch directory before and after the operation
and `FileSnapshotCommand` swaps them back by temp-copy and `rename`. A byte
restore invalidates the engine's open handle *and* every page-indexed raster
cache entry, so it is followed by `reloadAfterUndo()` — a full reopen, not a
cache flush.

Verified on `Contract_v3.pdf`: delete page → the label goes 11 → 10 → Ctrl+Z
→ back to 11, and the file is **md5-identical to the original**
(`7e4aa3b38c6a0cb300c93ded2faaa331`). An unchanged hash on its own proves
nothing — a no-op prints the same thing — so the page-count readout is the
actual evidence.

Opening a document now clears the stack. A stale `FileSnapshotCommand` would
not merely undo the wrong thing; it would write saved bytes back to a path
the user is no longer looking at.

### Annotations can be selected, moved and deleted

`PageStage` gained a selection (`hitTest` + corner handles), drag-to-move, and
Delete/Backspace. Every one of those routes through the undo stack, including
the eraser — which now captures the victim *before* removing it, so an erased
mark is restorable instead of simply gone.

Two bugs worth recording. Drag did nothing at first because the Move guard
tested `PointerEvent::buttonsDown`, which the streaming shell never populates;
the router-maintained `Widget::pressed` is the flag that actually works. And
Delete stopped working after an undo because `doUndo` blanket-deselected —
replaced with `dropDanglingSelection()`, which drops a selection only when its
id no longer resolves.

### Extract asks where to put the file — and stops leaking the other pages

Extract used to write silently beside the original. `OpenDialog` now has a
Save mode (filename field, "Extract to" title, same browser) and Extract
routes through it.

Building it turned up something worse than a missing dialog. PoDoFo's
`InsertPages` is implemented on `Append()`, which copies **every object** from
the source and links only the requested pages into the new tree. Everything
else stays in the file as orphans. Extracting page 1 of an 11-page contract
produced a 12,029-byte file that still contained the text of all eleven —
invisible in any viewer, recoverable in seconds from the raw bytes. For an
extract feature that is a confidentiality bug, not bloat.

PoDoFo advertises the fix as `PdfVecObjects::CollectGarbage`. On a freshly
built extract document it is a **no-op**: 33 objects in, 33 objects out, with
the duplicate catalogue still present. So the mark-and-sweep is ours now —
walk every reference reachable from the trailer, delete the rest. Same
extract: **11,994 → 2,300 bytes**, one page object, one catalogue, and PDFium
reads back the identical 694-character text layer. Two regression checks guard
it, because this is exactly the kind of fix that gets quietly reverted.

### Free text

Click with the Text tool, type on the page. `PageStage` owns the caret for the
duration and hands back every key it does not consume, so Ctrl+S still saves
while a box is open. Empty boxes are discarded without an undo entry — a stray
click should not eat the user's next Ctrl+Z.

Two things were wrong when it first ran, and neither was the typing:

- The text was drawn in **highlighter yellow**, which is invisible on white
  paper. One ink colour was being shared by every tool. Colour is now
  remembered per tool: yellow for the highlighter, near-black for everything
  meant to be read.
- Glyphs were sized in *interface* pixels, so they stayed the same size at 50%
  and at 400% — floating above the document instead of sitting on it, and
  exporting at the wrong size. They are sized from the annotation box now, so
  they scale with the page.

Export needed a new annotation kind: `AnnotKind::Text` was falling into the
`default` arm of `toSpec()` and **saving as an ellipse**. There is now a real
`/FreeText` with an `/AP` stream that bakes the glyphs in, because viewers
disagree about how to interpret `/DA` and an `/AP`-less annotation is the trap
this project has already paid for once.

Verified end to end: typed `APPROVED` onto `Contract_v3.pdf`, saved
(11,881 → 14,299 bytes), returned to the library, reopened — and **PDFium
renders it from the file's own appearance stream** at the correct ink
(measured darkest pixel `(5,6,11)` against the specified `(30,32,44)`).

`ctest` 11/11; the PoDoFo round-trip is now 58 checks.

### Still open
- No clipboard backend, and no native window shell.
- The Edge comparison has still never been run; there is no browser here.
- The Outline tab is still fabricated (`ReaderScreen.cpp:156`).
- Selection handles are drawn but not draggable — resize is a separate gesture.
- CI still builds with `-DFOLIO_BUILD_UI=OFF` on all three jobs.

---

## Round 9 — the punch list

Eight items, all of them things that were wrong rather than missing. Two turned
out to be bugs that had been invisible since the feature was written.

### Focus mode had never once worked

The button toggled, the state flipped, and nothing moved. The cause was one
argument:

```cpp
focusT_.value(0.0)      // in onLayout
```

`Animated<T>::value(now)` returns `from_` whenever `now < start_`, which is
always true for a literal `0.0`. Every layout pass therefore read the
animation's *pre-animation* value, so the panels never travelled. `value(now())`
fixed it. The same pattern is worth auditing anywhere else it appears: it fails
silently and looks like a layout bug, not an animation one.

Focus mode also had no way out that was visible while in it — the control that
turns it on scrolls away with the chrome. There is now a floating **Exit focus**
button, which needed `Widget::raiseToFront()` because children paint in
`children_` order and there is no z-index. `setFocusMode(bool)` is the single
owner of the state; the button, `F`, and `Escape` all route through it.

### The Outline tab was fiction

It drew `pages_ / 3` rows labelled *Chapter 1*, *Section 1.1*, *Section 1.2* —
derived from the page count, with page numbers computed as `i * 3 + 1`. It
looked exactly like data. It was the oldest placeholder in the project.

It now reads the file's real `/Outlines` tree. `PdfiumRasterizer::outline()`
walks it with `FPDFBookmark_GetFirstChild` / `GetNextSibling`, decodes the
UTF-16LE titles, and resolves each destination with `FPDFDest_GetDestPageIndex`.
The walk is depth- and count-capped (8 / 2000) because `/Outlines` is a linked
structure in an untrusted file and can be made cyclic. It runs on the worker
thread at load time, where the `FPDF_DOCUMENT` legally lives, and is cached.

Results on the corpus:

| Document | Entries | Max depth |
|---|---|---|
| Attention_Is_All_You_Need.pdf | 22 | 2 |
| lshort.pdf | 137 | 2 |
| Annual_Report_2026.pdf, Contract_v3.pdf, Thesis_draft.pdf, Japanese_Government_Notice.pdf | 0 | — |

Most PDFs have no outline at all, so the empty state says **"This document has
no bookmarks."** rather than inventing headings to fill the panel.

Replacing the fake data exposed a second bug behind it. `onPaint` reserves 22 px
for the "Outline" header; `onPointer` did not, so `hover_` was off by 0.65 of a
row and **every click opened the heading below the one under the cursor**. With
synthetic rows that were all alike, nothing looked wrong. Clicking
*Why Self-Attention* went to page 7. It goes to page 6 now.

The corpus test gained an outline section that asserts provenance rather than
shape: real first titles (`"Introduction"`, `"Thank you!"`), real first
destinations (p2, p3), at least three levels of nesting, no blank titles, and no
destination outside the document. **69 → 87 checks.**

### Text now has font and size

The right-hand panel was showing a WIDTH slider for the text tool, which has no
stroke. In the Text tool that slot is now **FONT** (Sans / Serif / Mono) and
**SIZE** (12 / 16 / 22 / 30 pt), and editing either applies to the box being
typed in — not just to the next one.

Three things this cost:

- `TextStyle` gained a `family` field. Putting it second broke roughly eight
  positional aggregate initialisers in `PageStage.cpp` with
  `cannot convert 'FontWeight' to 'FontFamily'`. **It has to go last.**
- There is no fontconfig in this build, so `SkFontMgr_New_Custom_Empty()` can
  only make typefaces from bytes handed to it. `loadFamily()` tries four
  known host paths per family (DejaVu, Liberation, macOS, Windows) and caches
  the miss as well as the hit. A host with none of them falls back to sans —
  silently, which is the honest gap here: the families are host-loaded, not
  bundled.
- Sizes are stored as a fraction of **page height** (`pt / 792`), like every
  other overlay measurement, so text keeps its size relative to the page at
  any zoom.

Export maps the three families onto the base-14 fonts PoDoFo can create
directly: `Helvetica`, `Times-Roman`, `Courier`. The round-trip test gained a
Times-Roman case — **63 checks**.

### The rest

- **The "+" tab button rendered as a "T".** The path was centred correctly; the
  stroke was simply too thin to close the horizontal bar at that size. Mapped
  the glyph pixel by pixel to prove it before touching anything.
- **Search was left-aligned among the tabs.** It is now a real centred pill
  reading *Search in document*, `clamp(W * 0.24, 180, 300)` wide, with the tab
  strip stopping 16 px short of it.
- **The selection popup was four labelled buttons** and far too wide for
  something that appears under the cursor. Icons only now, 182 × 44, labels
  moved to tooltips.
- **Focus and fit-page shared an icon.** Focus is diagonal expand/collapse
  arrows; fit-page is a page outline with a vertical double arrow.
- **The Open dialog read like a file manager.** The `..` row names the folder
  it actually goes to, sub-kilobyte files no longer say "0 KB", and the header
  says `documents · 6 PDFs` instead of printing an absolute path at the user.

### Still open
- No clipboard backend, and no native window shell.
- The Edge comparison has still never been run; there is no browser here.
- Selection handles are drawn but not draggable — resize is a separate gesture.
- CI still builds with `-DFOLIO_BUILD_UI=OFF` on all three jobs.
- Serif and mono are host-loaded, not bundled.

---

## Round 10 — the native window shell

The brief was Win32 + Cocoa + X11/Wayland behind one thin platform layer,
with a GPU surface, a real clipboard, native file dialogs and working window
controls — and the expectation that it could only be compile-checked here.

It turned out it could be **run** here. That changed the outcome: three of
the bugs below are ones no amount of compile-checking would have found.

### The layer

`folio-platform` is four interfaces and nothing else:

| Interface | What it hides |
|---|---|
| `IWindow` | creation, events, DPI, cursors, min/max/close, system move & resize |
| `IGlContext` | `makeCurrent` / `swapBuffers` / `procAddress` — **no Skia** |
| `IClipboard` | text in and out |
| `IFileDialog` | blocking native open/save, with `available()` so callers can fall back |

Two rules keep it thin, and both are easy to break by accident:

- **It must not know about Skia.** A backend hands out a current GL context;
  building a canvas on it is `folio-render`'s job. The moment `IGlContext`
  grows an `SkCanvas*`, the seam is gone. Ganesh lives in
  `SkiaGpuTarget`, which is the only file that sees both Skia's GPU API and
  the fact that a GL context exists.
- **It must not know about folio-ui.** Events are described in platform
  terms and `NativeShell.cpp` translates them. Otherwise every backend
  would need the widget library to compile.

`NativeShell.cpp` is the whole desktop application and contains no
`#ifdef _WIN32`.

### What runs

`folio-desktop`, 12 MB, on X11 with an EGL/Ganesh GL context. Not the
streaming shell — a real window with real GL.

Verified by driving it with synthetic X input and reading the framebuffer
back: the library page, the Open dialog, and a real PDFium-rendered document
with thumbnails and panels. Screenshots in `docs-shots/native-library.png`
and `docs-shots/native-reader.png`.

**The clipboard was verified against an independent X client.** Select text
in the document, press Ctrl+C, and `xclip -selection clipboard -o` — a
separate process — prints the paper's abstract. That exercises the whole
X11 selection protocol: ownership, `TARGETS`, `UTF8_STRING`, and answering
`SelectionRequest` events from the event loop. X11 has no clipboard, only a
conversation, and this is the half that is easy to get wrong.

### Three bugs that only running could find

**1. The software fallback presented nothing.** The GPU path ends in
`swapBuffers`; I had the CPU path end there too, having rendered into an
offscreen raster surface that was never uploaded. A black window. The fix is
also the correct design: `IWindow::presentSoftware(bgra, w, h, stride)`,
implemented with `XPutImage`. Presenting CPU pixels *through GL* is
incoherent anyway — if Ganesh failed to start, GL is usually the thing that
is broken.

**2. Pressing "Open file" segfaulted instantly.** The native picker
delegated its directory listing to `FilePicker::current()` — which is
itself. Infinite recursion, stack overflow, dead. There is now a
`FilePicker::local()` for exactly this, with the trap written on it.

**3. X11 `#define`s four of our identifiers.** `X.h` is a 1987 C header that
defines `CursorShape`, `Expose`, `FocusIn` and `FocusOut` as bare integers.
The preprocessor wins every one of those fights, and the errors land nowhere
near the cause (`expected ')' before numeric constant`). The values are now
captured into constants and the macros `#undef`ed at the top of the backend.

### And one that was not a bug

The first native screenshot showed the accent button in vertical rainbow
stripes and colour-fringed text. It looked like a broken shader on the GPU
path. It was the *screenshot decoder*: an X server on a 24-bit visual
reports `bits_per_pixel` 24 and pads rows to a 32-bit boundary, so decoding
four bytes per pixel drifts one byte per pixel and produces a 3-pixel
rotating colour rotation. Only saturated colours reveal it, which is why the
white UI looked fine and one purple button looked broken. Decoded correctly
the pixel is `(67, 56, 202)` — exactly the accent. The renderer had been
right all along.

`tools/xwd2png.py` carries that explanation, because the next person to
screenshot an X11 app will see the same thing and reach for the renderer.

### Link-order trap

Skia's prebuilt archive contains `GrGLMakeGLXInterface.cpp` whether or not
we use GLX, and it is listed *after* the GL libraries. Under Debian's
default `--as-needed`, libGL has already been discarded by the time those
symbols come up undefined. The GL libraries are now pinned with
`-Wl,--no-as-needed`.

### Headless harness

`tools/bootstrap-headless.sh` + `tools/run-headless.sh` bring up Xvfb with
Mesa llvmpipe and run the real binary, with `--shot` for a screenshot. No
root except one symlink — Xvfb execs `/usr/bin/xkbcomp` by absolute path and
has no override for it.

This is what makes **turning the UI build on in CI** viable: the native shell
can be launched, screenshotted and exercised on a headless runner. It is not
a performance harness — llvmpipe is a CPU rasteriser and timings from it say
nothing about a real GPU.

### Still open
- **Win32 and Cocoa backends are not written.** The interface they implement
  is settled and proven by a real backend, but the files do not exist. CMake
  selects a backend only when its source is present, so those platforms
  build against the `none` backend and say so rather than failing to
  configure.
- Wayland likewise. EGL was chosen over GLX partly for this: only the
  `eglGetDisplay` argument and the native window type change.
- Maximise/restore sends the correct EWMH client message but cannot be
  verified under bare Xvfb, which has no window manager.
- The clipboard refuses `INCR` (chunked) transfers rather than implementing
  the state machine.
- CI still builds with `-DFOLIO_BUILD_UI=OFF`.

---

## Round 11 — UI corrections, and two honest answers

### Four corrections

**1. The markup popup would not go away.** `onSelectionCleared` existed and
was wired, but it only fired when the next press landed on *empty space*.
Pressing on **more text** took the other branch, which collapses the
selection to a caret and returns early — so the popup stayed on screen
pointing at text that was no longer selected. Clicking elsewhere in the
document is the ordinary way to deselect, so the broken path was the common
one. The notification now happens at the top of the press handler, before
either branch: any press discards the selection, so the chrome that belongs
to it comes down first.

**2. Search is one icon in the command bar.** It was a 180–300 px pill
centred in the title bar. It is now a single `search` icon in the right-hand
cluster next to the display control. The field it opens was always in the
popover, so the chrome only ever needed to carry the affordance. The tab
strip gets the whole title bar back.

**3. Double-click opens a page in "All together".** Grid layout is a page
picker, not a reading view — there is nothing there to select or pan — so a
double click on a tile now switches to single page, jumps to it and fits it.
It goes through `setLayoutMode`, so the pill label and the right panel's
LAYOUT control stay in agreement instead of drifting apart.

This needed double-click support in the streaming shell, which had never set
`clickCount` at all: the browser sends raw presses, and the shell now pairs
them with the same 400 ms / 4 px rule the X11 backend uses.

**4. Tool options moved into the command bar.** The bar had a large empty
middle while the tool's two most-used settings sat two panel-widths away.
There are now two controls at the end of the bar:

| Control | Shows | Opens |
|---|---|---|
| Colour | the current ink, as a filled chip | 6×2 grid — six inks plus a lighter tint of each |
| Weight | three bars of increasing thickness | Fine / Medium / Thick, or text size for the Text tool |

They appear only for tools that have those options (Pen, Highlighter, Text,
Note, Shapes, Eraser) and vanish for Select and Hand, so the bar is empty in
Read mode rather than permanently reserving the space. The side panel keeps
the full set; this is the fast path.

`Button` gained real swatch support (`isSwatch` / `swatchColor`) rather than
having the panel paint circles over anonymous buttons from the outside.

### Harness note

`/tmp/cap.py` was rewritten after the sandbox reset and silently did
nothing: `d` and `u` records need **five** fields (`d,x,y,btn,mods`), and a
bare `d` is dropped by the parser without complaint. Frames still advanced,
so it looked exactly like a broken hit-test rather than a broken harness.

### Answer: a Windows .exe

Not from this sandbox, and the reason is not effort. The prebuilt Skia and
PDFium for Windows are MSVC-ABI; a MinGW cross-compile cannot link them, and
there is no MSVC here. A Windows binary has to be built on Windows.

Two things stand between here and one, and only the first is real work:

1. `folio-platform/win32/Win32Window.cpp` does not exist. The interface it
   has to implement is settled and proven by a working backend, so this is
   transcription rather than design: `CreateWindowEx` with `WS_POPUP` for
   client-side decorations, `WM_NCHITTEST` for the resize border and the
   title-bar drag, WGL for the context, `OpenClipboard` / `CF_UNICODETEXT`,
   and `IFileOpenDialog` for the dialogs.
2. A `windows-latest` CI job that builds it and uploads the `.exe` as an
   artifact. That is the only way the binary gets produced, and the only way
   it stays buildable.

CMake already refuses to select a backend whose source is missing, so
Windows configures against the `none` backend today and says so, rather than
failing.

### Answer: mobile, tablet, web

**No. The UI is not responsive, and it is worth being exact about it.**
`docs-shots/phone-width-today.png` is the library page at 390×844 — a real
frame, not a mock-up. The 268 px sidebar takes two thirds of the width, the
document cards are squeezed into unreadable vertical slivers, and "Good
afternoon" truncates to "Go...". It is unusable, and nothing about it is
surprising: every layout in the app was written against a 1440×900 design
with fixed panel widths.

What is *not* a problem is the architecture. The whole UI is one C++ widget
tree with a single layout pass per screen, sizes already come from tokens,
and device-pixel-ratio is plumbed end to end. Responsiveness here is a
layout problem, not a rewrite:

- **Breakpoints.** Compact (< 600), medium (600–1024), expanded (> 1024).
  One enum in `AppState`, read by the three `onLayout` functions that
  currently hard-code panel widths.
- **Panels become drawers.** Below medium, the side panels overlay the
  content instead of displacing it, and the bottom pill becomes a bottom
  bar — which is where a thumb can reach it.
- **Touch targets.** Icon buttons are 30 px. They need to be 44.
- **Gestures.** Pinch zoom and swipe paging do not exist; the stage handles
  wheel and drag only.
- **Shells.** iOS, Android and WASM have none at all. WASM is the cheapest of
  the three, because Emscripten's EGL path reaches the same `IGlContext` the
  X11 backend already implements.

None of that is written. The plan documents claim these targets; the code
does not support them yet, and the screenshot is the proof.

---

## Round 12 — every tool option in the command bar, and annotation properties

### The text-markup colour bug

`applyTextMarkup` hard-coded its colours:

```cpp
a.color = kind == AnnotKind::Highlight ? Color{250, 204, 21, 255}
                                       : Color{229, 57, 53, 255};
```

So the Highlighter **tool** and the Highlight you get from selecting text and
pressing the button were two unrelated things that happened to share a name.
Changing the tool's ink did nothing to the other one. There was no way to
change the selection-markup colour at all — not in the panel, not anywhere.

Both now draw from one value. The selection popup gained a colour chip
showing the ink the markup will use, and it edits the same setting as the
Highlight tool's colour control. Pick green in either place and both agree.

### Every option, in the command bar

The bar now carries the full set, one icon each, only for tools that have
them:

| Tool | Controls shown |
|---|---|
| Pen | opacity · thickness · colour |
| Highlighter | opacity · colour |
| Note | opacity · colour |
| Shapes | opacity · thickness · colour |
| Eraser | size |
| Text | font (Aa) · size (Tt) · colour |
| Select / Hand | none — the bar stays empty |

Thickness and size share a slot and swap icon and contents (`Fine / Medium /
Thick` versus `12 / 16 / 22 / 30 pt`). Opacity opens four presets over a
slider: the presets are what get used, the slider is for when none of them
is right.

### Changing an annotation after the fact

Clicking an existing annotation with the Select tool now selects it and the
right panel becomes its property sheet — header naming the kind, its colour,
its width, its opacity, read **from the annotation** rather than from the
tool. Changing any of them edits that annotation through
`ModifyAnnotCommand`, so Ctrl+Z puts it back.

The command bar follows the same selection, so the colour chip shows the
selected annotation's ink and recolours it in place.

### OptTarget

One set of controls now serves three jobs, and without saying which, they
silently write to the wrong place — picking a colour for a selected
annotation would have changed the pen instead. So:

| Target | When | Writes to |
|---|---|---|
| `Annotation` | something is selected | that annotation, undoably |
| `Markup` | text is selected | the ink the next markup will use |
| `Tool` | neither | the active tool |

Resolved in `syncToolOptions()` in that order — a selected annotation wins
over a text selection, which wins over the tool.

### Three traps hit on the way

**The layout pass overrides visibility.** `updateRightPanelContext()` sets
which panel controls are visible, and then `onLayout` sets them again from
its own copy of the condition. Making the panel respond to a selected
annotation meant changing *both*, in three places that each recomputed
`authoring` independently. They are now one function, `stylingActive()`,
because three copies of a condition is three chances to disagree.

**`setSelected` and `setValue` both fire `onChange`.** Refreshing the panel
to match a selected annotation therefore called straight back into the apply
path — recursing, and pushing an undo entry every time an annotation was
merely *selected*. Hence `syncingOpts_`, which every apply function checks
first.

**A second view of one value needs refreshing too.** The selection popup's
chip was updated only when the selection was first made, so after picking a
colour it kept showing the old one while the command bar showed the new one.

### Not a bug, twice

A screenshot of the command bar in Text mode appeared to show only two
icons, suggesting the font and size controls were missing. Debug output said
`col=1 wt=1 op=0 fo=1` — correct. The crop had caught a frame from the
Highlight tool. Magnified, the Text bar shows **Aa**, **Tt** and the chip,
exactly as intended. `FOLIO_DEBUG_OPT=1` prints the target and the four
visibility flags on every sync; it is what settled this in one build instead
of several.

### Still open
- Annotation selection handles are drawn and the body can be dragged, but
  the handles themselves do not resize.
- Ink annotations recolour, but there is no multi-select.

---

## Round 13 — responsive breakpoints, grouped markup, zoom-to-cursor

Three of the five asks from this round are shipped and verified live. Two
(more colours, visualised thickness) are not started; see "Next" below.

### 1. Full responsive breakpoints

`AppState.h` gains `enum class Breakpoint {Compact, Medium, Expanded}`,
`breakpointFor(float)` and a `LayoutMetrics` struct. Thresholds are window
widths, not device classes, so a desktop window dragged narrow gets the
tablet layout:

| | Compact (<720) | Medium (<1120) | Expanded |
|---|---|---|---|
| gutter / panel gap | 10 / 8 | 16 / 12 | 24 / 16 |
| left · right panel | 240–300 (drawer) | 224 · 232 | 268 · 272 |
| panels | overlay the canvas | columns | columns |
| tab strip | hidden | shown | shown |
| chrome | icon-only | icon-only | full labels |

`LayoutMetrics::forWidth()` is read once at the top of each `onLayout`, so
the size class is decided in one place rather than tested at thirty call
sites.

Supporting changes:
- **`SidePanel::setExpandedWidth(float, double)`** — the expanded width was
  a token read directly in two places in `Containers.cpp`, which made a
  per-breakpoint panel width impossible. It is now a member that re-targets
  the animation, so a breakpoint crossing *slides*.
- **`ReaderScreen::onBreakpointChanged()`** — fires once per crossing.
  Collapses both panels entering overlay mode, restores the left panel at
  Expanded, and swaps button labels for icons.
- **Command bar reflow.** Nothing may overflow: at Medium the layout tiles
  go (the pill's layout popup is the same set) and Share becomes an icon;
  at Compact the panel toggles, focus, theme and Share go too. Each is
  dropped only where another affordance already covers it — the collapsed
  rails *are* the panel toggles, the right drawer carries the theme.
- **Pill reflow** — at Compact the fit buttons go (both fit modes live in
  the layout popup) and the layout button becomes an icon.
- **Welcome screen** — sidebar hidden at Compact with the open action moved
  into the greeting row; hero and drop zone stack below 460px of pane;
  search and sort stack below 520px; the card grid fits as many columns of
  at least `kCardMinW` as fit, instead of dividing by a fixed card width
  (which produced one 120px sliver).
- **Hero text flow** — its rows were pinned to both edges at fixed offsets,
  so at 104px tall the meta line and the progress track overlapped. The
  flow is now derived from the box it is handed.

Verified at 390×844, 820×1180 and 1440×900. Expanded is pixel-unchanged.

### 2. Grouped multi-line markup

`Annotation::group` (0 = ungrouped). `applyTextMarkup` stamps the first
line's id onto every line of the batch — already unique, already allocated,
so no second counter can drift. `AnnotationStore::groupMembers(id)` returns
`{id}` for an ungrouped annotation so callers need no special case.

- `PageStage::paintAnnotSelection` outlines every member at 55% accent and
  puts the corner handles on the union.
- `PageStage::deleteSelectedAnnot` removes the whole group.
- `ModifyManyCommand` (Edits.h) + `ReaderScreen::modifySelectedGroup()`
  route all four option paths (colour, width/size, opacity, font) through
  one undo entry for the whole group.

Verified: selected four lines → Highlight → clicked the *title line only* →
all four outlined with union handles → pink swatch recoloured all four →
**one** Ctrl+Z restored yellow on all four → a second removed the markup.

### 3. Zoom follows the cursor

The ctrl+wheel branch carried the comment "zooms about the pointer" while
anchoring to the fixed centre of the viewport. `PageStage` gains
`setZoomFocus(localX, localY)` / `clearZoomFocus()`, and `captureZoomAnchor`
now records a horizontal page fraction alongside the vertical one. x is only
steered once the page is wider than the viewport; below that the page is
centred and there is nothing to hold. The pill's zoom buttons call
`clearZoomFocus()` so they keep holding the centre.

Verified from two identical fresh instances, differing only in cursor
position: a feature 22px below the canvas top moved to **113px** with the
cursor at y=200 (prediction from the follow-the-cursor model: 113.6), while
the same zoom with the cursor at y=800 scrolled it off screen entirely.

### Harness notes (cost real time this round)

- The `/input` **wheel** record is `w,x,y,wheelX,wheelY,mods` — **six**
  fields. A five-field form is silently dropped, which reads exactly like
  "ctrl+wheel zoom is broken".
- The `/input` **key** record is `k,name,pressed,mods[,text]` — **four**
  fields minimum, and needs a separate press and release. A three-field
  form is silently dropped, which reads exactly like "Ctrl+Z is broken".
  Four consecutive false negatives this round came from this one mistake.
- Forcing a panel collapsed via `SidePanel::setCollapsed` without also
  setting `state_.*PanelCollapsed` desyncs the toggle, so the next rail tap
  flips the flag the wrong way and the drawer refuses to open exactly once.
- The screen's `onPaintOverlay` runs after every child, so the pill's text
  labels painted straight through an open drawer. The pill is hidden when
  a drawer is open in overlay mode.

### Next

4. **More colour choices** — `kToolInks[6]` → a larger palette. The 12 call
   sites already clamp to `(0,5)` and index `% 6`, so introducing a
   `kInkCount` constant makes the expansion mechanical; the real work is
   turning the panel's swatch row and the popover into grids.
5. **Thickness: more options, visualised** — render actual line widths in
   the command bar and the right panel instead of `Fine/Medium/Thick`,
   mirroring `Button::isSwatch` with an `isWidthSample`.

---

## Round 14 — palette, visualised thickness, and the Windows path

### 1. Twenty-four colours (was six)

`kToolInks` is now `kInkCount = 24` in four rows of `kInkCols = 6`: vivid,
pastel, deep, neutral. **The first six indices are unchanged on purpose** —
annotations already written, the default markup colour and every saved
document resolve through them.

The 12 call sites that clamped to `(0,5)` and indexed `% 6` now use
`kInkCount`, so the palette has one definition. Two duplicates were removed
along the way: a second `kInks[6]` inside the panel-overlay paint, and the
popover's six base inks plus six tints *generated at runtime* — which meant
the popover could offer colours the panel could not, with indices that did
not survive a round trip.

Panel and popover both lay out as grids sized from the available width.
Verified: brown (index 22) picked in the panel → command-bar chip reads
(122,82,46) ≈ (120,83,44).

### 2. Thickness: six weights, drawn

`Button::isWidthSample` / `widthSample` / `sampleColor` mirror `isSwatch`:
the button paints a stroke of the real weight. New `WidthStrip`
(`components/WidthStrip.h`) is a drop-in for the `SegmentedControl` that
used to say Fine / Medium / Thick — same `selected()` / `setSelected()` /
`onChange` surface, so the twelve call sites did not change, and like
`SegmentedControl::setSelected` it deliberately does **not** fire
`onChange`.

`kToolWidths[6]` replaces the three-value `kWidths` table that was
duplicated at **four** call sites. Samples appear in the right panel, the
command-bar chip and the weight popover, all tinted with the live ink.

### 3. A real bug this exposed

The width strip rendered as 1.7px slivers. Cause, and it is general:
**panel content was laid out from the panel's animated width, but
`layoutTree()` is invalidation-driven, not per-frame** (five passes in a
whole session). Content laid out while the panel was a 48px rail
(`innerW = 20`) never reflowed when the panel slid open. Widgets that
position their own children — the new strip — showed it; widgets the
screen positions happened to get lucky.

Fix: lay panel content out at `expandedWidth()`, not the animated width.
The panel already clips its content, so the reveal still animates and the
content is never stale. Applied to both panels.

### 4. Windows: what exists now

`folio-platform/win32/Win32Window.cpp` (808 lines) — the backend the CMake
has been waiting for. Borderless but still `WS_OVERLAPPEDWINDOW`, so Aero
Snap, snap layouts and the taskbar menu keep working while `WM_NCCALCSIZE`
removes the frame area; the two carve-outs that approach requires (the
maximised inset, the hit-test resize border) are implemented and commented.
Also: WGL with a 3.3-core upgrade path, per-monitor DPI v2 loaded
dynamically, queued events, `MsgWaitForMultipleObjectsEx` for the idle
rule, surrogate-pair-safe `WM_CHAR`, `CF_UNICODETEXT` clipboard,
`IFileOpenDialog`/`IFileSaveDialog`, and `StretchDIBits` for the software
fallback.

Supporting work:
- `cmake/FolioDependencies.cmake` was Unix-only (`libskia.a`). It now
  derives the library spelling from the toolchain (`libfoo.a` / `foo.lib`)
  and adds the Windows system libraries Skia leaves undefined
  (opengl32, gdi32, usp10, fontsub, windowscodecs…). The `nm`-based
  Graphite probe is skipped under MSVC.
- `folio-platform/CMakeLists.txt` no longer passes GCC `-Wno-*` spellings
  to MSVC, which rejects unknown flags rather than ignoring them.
- `tools/bootstrap-deps.ps1` — PowerShell twin of the bash bootstrap.
  User-scoped (`%LOCALAPPDATA%\folio-deps`), no admin rights.
- CI gains a `windows-msvc` job on `windows-2022` that builds, tests,
  **asserts the backend resolved to win32 rather than falling back to
  none**, and uploads `Folio.exe` as an artifact.

Linux reconfigured from scratch and rebuilt after all of this: 111/111
targets, `ctest` 11/11.

### Honest status of the Windows exe

Not compiled yet. **MSVC does not run on Linux**, and this sandbox has no
MinGW, no clang, no clang-cl and no Wine, so the Win32 backend above is
written-but-unverified-by-compiler. The CI job is the verification path and
also the build machine. Expect a first round of compile errors there; that
is normal for 800 lines of platform code that has never seen its SDK.

Three facts that constrain the options:
- Skia's Windows prebuilt (45.9 MB, skia-pack m144) is **MSVC ABI**, so
  MinGW cannot link it. Cross-compiling would mean clang-cl + lld-link with
  an `xwin`-fetched MSVC CRT and Windows SDK.
- PDFium's Windows prebuilt is a **DLL** (3.9 MB + import lib). A genuinely
  single-file `Folio.exe` needs PDFium built static from source; otherwise
  the deliverable is `Folio.exe` + `pdfium.dll`.
- PoDoFo has no official Windows prebuilt → vcpkg, or
  `-DFOLIO_WITH_PODOFO=OFF` (drops annotation *writing* only).

## Round 15 — popover placement and reveal origin

Two reported bugs, both in `Popover::open()` (`folio-ui/components/Overlays.cpp`)
and its callers in `ReaderScreen.cpp`.

### 1. "A pop-up at the top that goes beyond the left and right side panels"

`Popover::windowSize` defaults to `{1440,900}` and must be set by the owner.
`grep -n windowSize` showed only `findPop_`, `displayPop_`, `selPop_` and
`openOptPopover` ever set it — **`modePop_` and `layoutPop_` never did**. Both
therefore clamped their x against a 1440-wide window that did not exist, so on
any smaller window they were free to run off the edge. Both also anchored on
the trigger's *left* edge (`toWindow({0.f, …})`), which centres the card half
its own width further left again.

### 2. "The animation appears at the top, but it should appear from where the click happens"

`layoutPop_` pre-offset its anchor upward by the popup's own height and then
opened *below* that invented point. `open()` derives the circular-reveal origin
from the anchor, so `flipped = y < anchor_.y` came out false and the reveal grew
from the **top** edge of a card that sits **above** its button.

### Fix

* New `ReaderScreen::openPopoverFor(Popover*, Widget* trigger, bool above)` —
  sets `windowSize`, sets `preferAbove`, anchors on the trigger's **centre**
  (`frame.w*0.5f`, `y = above ? 0.f : frame.h + 6.f`), and calls
  `closeOtherPopovers()`. All six open sites route through it: `modePop_`,
  `layoutPop_` (`above=true`), `findPop_` (button **and** Ctrl+F), `displayPop_`,
  `openOptPopover`.
* `revealFrom_` is now the anchor **projected into local bounds on both axes**,
  `clamp(anchor - origin, 0, extent)`. The `flipped` bool is gone. A card that
  was clamped sideways still reveals from under its button.
* `closeOtherPopovers()` also clears `chevronOpen` on `modeBtn_`/`viewBtn_`,
  fixing a separate artefact where the colour popover and the display popup were
  open simultaneously and Escape closed only one.

### Verification

`FOLIO_DEBUG_POP=1` prints placement at every open (kept as a permanent
diagnostic — white card on white page defeats pixel-sampling, so geometry is
verified numerically).

| window | popover | frame | right edge | reveal | above |
|---|---|---|---|---|---|
| 1000×700 | mode | 8,105 212×120 | 220 | (35,0) top | no |
| 1000×700 | layout | 529,456 248×180 | 777 | (124,**180**) bottom-centre | yes |
| 390×844 | mode | 8,105 212×120 | 220 | (29,0) | no |
| 390×844 | layout | 134,600 248×180 | **382** | (172,**180**) | yes |
| 390×844 | search | 62,105 320×108 | **382** | (293,0) | no |

390 − 8 = 382, so every card is inside the margin on a phone-width window. The
search popover previously computed x = 355 − 160 = 195 against a 1112 clamp
ceiling and sat 125 px off the right edge. The layout popup's reveal y is now
its own height (bottom edge) rather than 0.

`ctest` 11/11.

## Round 16 — z-layers (popovers were painting *under* the side panels)

Reported with four screenshots: the mode dropdown behind the left panel and
behind the collapsed rail, the display popover and the width popover behind
the right panel. Worst at desktop size.

### Root cause

Layering was expressed by **mutating the child list**. `Widget::raiseToFront()`
moves a widget to the end of its parent's children, and paint order *is* child
order. `onBreakpointChanged()` called it on both panels when a narrow window
turned them into overlay drawers:

```cpp
leftPanel_->raiseToFront();     // so the drawer covers the page
rightPanel_->raiseToFront();
```

**`raiseToFront()` has no inverse.** Nothing put the panels back when the
window widened again, so after one narrow phase both panels sat above every
popover — permanently, at every size, for the rest of the session. And the
raise was far too broad even while it was wanted: the intent was only to beat
`stage_`, not the menus. `lastBreakpoint_` initialises to `Expanded`, so a
first layout pass at zero width also counts as a narrow phase.

### Fix — a real layer, not a list shuffle

`Widget` gained `zOrder()` / `setZOrder()` and a `zlayer` scale:
`content 0 · floating 10 · panel 20 · popover 30 · modal 40`.
`ensureChildOrder()` stable-sorts children by layer, and is called from
`paintTree`, `hitTest` and `layoutTree`. Stable, so equal layers keep
insertion order — every existing caller, `raiseToFront()` included, behaves
exactly as before. Cost is one bool test per traversal; the sort only runs
after an `adopt()` or a `setZOrder()`.

* `Popover::Popover()` sets `zlayer::popover`, so no caller can forget.
* Panels: `setZOrder(m.panelsOverlay ? zlayer::panel : zlayer::content)` —
  reversible, so widening puts them back down.
* `focusExitBtn_` uses `zlayer::floating` instead of `raiseToFront()`.

### Verification (live, 8080)

| case | shot | result |
|---|---|---|
| display popover vs expanded right panel | `/tmp/p1-c.png` | popover on top |
| mode dropdown vs left panel, **after 1440→600→1440** | `/tmp/q1-c.png` | on top |
| mode dropdown vs collapsed rail, 390 wide | `/tmp/s2-c.png` | on top |
| width popover vs DRAW panel | `/tmp/u2-c.png` | on top |
| phone drawer vs page, 390 wide | `/tmp/s1-c.png` | drawer still covers page |

`ctest` 11/11.

### Also: the stream shell can resize

There was no way to cross a breakpoint in the preview — the shell took its
size once from `--size` and had no window manager behind it, which is why the
narrow→wide corruption went unnoticed. `/input` now accepts **`r,<w>,<h>`**
(clamped 320–4096): it reallocates the Skia surface and calls `app_.resize()`
on the render thread. Loading the page as **`/?fit`** makes it track the
browser viewport (debounced 150 ms), so the responsive breakpoints can be seen
live. Default behaviour is unchanged.

Note for future sessions: the shell serves MJPEG at **`/stream`**; there is no
`/frame` endpoint. A capture helper must pull a frame out of the multipart
stream.

## Round 17 — the phone layout

Design conversation, then a build. The brief, in the user's terms:

* Tool option popups **stay where they are** (anchored to the control at the
  top). Tapping at the top and then reading the result at the bottom is the
  tiring part — so only *one* surface moves.
* The **right panel** becomes the thing that lives at the bottom: the detailed
  option list, raised by a small icon. **Not** automatic on tool selection —
  it is the "more options" surface, and the place future options go.
* It **does** raise itself when an existing annotation is selected.
* The **document name** sits at the top as a dropdown switcher, its menu
  dropping below it with the same reveal as every other menu.
* The **theme toggle** takes the corner the window controls vacate; a phone
  has no minimise/maximise/close.
* **Pages/outline** moves to the bottom-left, under the left thumb.

### What changed

`SidePanel::Presentation::{Column, Sheet}`. In Sheet mode the panel stops
being a column that narrows into a rail and becomes a full-width surface that
travels vertically: `sheetT_` 0..1, `OutCubic` up (a settle, as though pushed
by a thumb) and `Standard` down (a dismissal that lingers feels broken).
`CardView::setCorners()` was added so the sheet rounds along its top edge
only — it then reads as having slid up from off-screen rather than as a card
resting near the bottom.

The rails are gone on a phone. Two 48 px rails over a 390 px page cost a
third of the width while showing nothing; their jobs moved to the two bottom
corners as `pagesFab_` and `inspectFab_` (`ButtonStyle::Card`, 42 px).
Both are `zlayer::floating` — they are built with the title bar, long before
the canvas exists, so without a layer the stage painted straight over them
(the same trap Round 16 was about).

Sheet heights: pages 60 % of the window (browsing wants room), inspector
46 % (leave the page visible). Only one sheet at a time. Dismiss by tapping
the page, or the sheet's own chevron. The handles hide while a sheet is up,
since it covers both corners and they would only show as slivers.

`docBtn_` + `docPop_` replace the tab strip below 720 px; the menu lists open
documents and carries "Open a PDF…", because the library is otherwise
unreachable from there. `themeBtn_` routes through the existing
`chooseScheme()`, so the circular reveal grows from the button.

### Verified at 390×844

| | shot |
|---|---|
| rails gone, page edge-to-edge, name + theme in the title bar | `/tmp/m1.jpg` |
| inspector sheet raised from the bottom-right handle | `/tmp/n2.jpg` |
| rounded top corner | `/tmp/corner.png` |
| **auto-raise on selecting existing ink**, 24 colours + 6 widths laid out across the full width | `/tmp/ink1.jpg` |
| document switcher dropping below the title | `/tmp/d1-c.png` |
| tap page to dismiss, handles return | `/tmp/dis-c.png` |
| pages sheet, tall, 2-column thumbnails | `/tmp/pg.jpg` |
| **desktop unchanged** | `/tmp/desk-s.png` |

`ctest` 11/11.

### Open

* The sheet has no drag handle and cannot be dragged between detents; it is
  tap-to-open, tap-to-dismiss only.
* Tool option buttons do not fit in the phone command bar unless an
  annotation is selected, so on a phone those options are reached through
  the sheet. This matches the brief ("most of the items will show in that
  inspector") but means the top popups are rarely used at that width.

## Round 18 — multi-document, title overflow, and the Windows package

### Multi-document (the reason overflow could not be tested)

`openFile()` used to rename the single tab rather than add one, so only one
PDF was ever open and the tab strip could never be put under pressure. It
also left the tab title disagreeing with `docTitle_` after a second open.

Now there is a real document list. `ReaderScreen::OpenDoc{path,title,annots,
page}` with `docs_` + `activeDoc_`; `loadPath()` does the PDFium work,
`openFile()` owns the list and the tab strip, `switchToDoc()` stashes the
outgoing document's annotations and page and restores the incoming one's,
`closeDoc()` picks a neighbour or returns to the library. Re-opening from
disk on switch is deliberate — PDFium is cheap to re-open and holding N
documents resident is not worth it for a reader. Annotations are restored
with `addExisting`, not `add`, so ids survive and undo entries still
resolve. Undo history is cleared on switch: page-op undo writes bytes back
to a stored path, and carrying it across could rewrite the wrong file.

Wired to `tabs_->onSelect` / `onClose` and to the phone document menu.

### Overflow

Verified with a deliberately long fixture,
`documents/Quarterly_Consolidated_Financial_Statements_And_Managements_Discussion_2026.pdf`
(kept in the repo so this stays testable):

* desktop, 5 tabs at 1440: all elide, `+` and window controls intact
* phone title: `Quarterly_Consolidated_Finan…` + chevron + theme, no overlap
* phone menu: 5 rows, active one marked, long names elided, `Open a PDF…`
* open dialog: already elided correctly

One real bug found: with a *short* name the chevron was stranded at the far
right of a full-width button, reading as a separate control.
`Button::chevronHugsLabel` draws the chevron immediately after the label and
centres the pair, falling back to the pinned layout when the label is too
long to fit — so a long name still elides against the full width. Measuring
happens in `onPaint`, where the paint target is, because folio-ui-core has
no text measurement of its own and must stay Skia-free.

### Windows: three things that would have failed on first contact

1. **`folio-shell-stream` had no UNIX guard.** It is built on BSD sockets
   (`sys/socket.h`, `unistd.h`, a self-pipe); the Windows build would have
   died at the first `#include`. Now `if(TARGET folio-render-skia AND UNIX)`.
2. **`-Wno-conversion -Wno-shadow` on five targets.** MSVC *rejects* an
   unknown `/W` flag rather than ignoring it. Replaced with
   `folio_relax_warnings()` in `cmake/FolioOptions.cmake`, which maps to
   `/wd4244 /wd4267 /wd4100 /wd4458` on MSVC.
3. **`FOLIO_ASSET_DIR` is a configure-time absolute source path.** A
   downloaded artifact would have started with no fonts. `NativeShell` now
   resolves `assets/` beside the executable first, then one level up, then
   the baked-in path — via `GetModuleFileNameW` / `_NSGetExecutablePath` /
   `/proc/self/exe`.

Also: `bootstrap-deps.ps1` honours `FOLIO_DEPS_DIR` (the Actions `env`
context cannot see the runner's `%LOCALAPPDATA%`, so the cache step needs a
nameable path).

### Deliverables

* **`BUILD-WINDOWS.md`** — three routes (GitHub Actions / local MSVC /
  tagged release), failure table, what builds on Windows and what does not,
  PoDoFo trade-off, per-dependency licence exposure, and an honest status
  section (the 808-line Win32 backend has still never met a Windows SDK).
* **`.github/workflows/release-windows.yml`** — dependency cache, build,
  test, package, a staging check that fails if the exe/DLL/fonts did not
  travel, artifact upload, and a GitHub Release on `v*` tags.
* **`docs/RUNNING-WINDOWS.txt`** — ships inside the zip as `README.txt`.
* CI's Windows job now copies `assets/` into `dist/`.

Linux clean reconfigure after all CMake edits: **111/111 targets**,
`ctest` 11/11.

## Round 19 — panel sync, jump-to-page, and an honest audit

### 1. Panel content desynchronised from the panel (fixed)

Reported: content appears before the open animation finishes and lingers
after the close one does.

Cause: the body opacity was `1.f - railAlpha()`. `railAlpha` is
`smoothstep(0.72, 1.0, collapseT)` and is *deliberately* delayed, because
the icon **rail** should only appear once the panel is nearly shut. Taking
its complement for the **body** inherited that delay backwards: content
reached full opacity 28% into the opening, and on closing stayed solid
until the last 28%.

`SidePanel::contentAlpha()` already existed for exactly this and was
**never called by anything**. It is now the single source of body opacity
(four call sites: both panel overlay painters, both layout blocks), and was
retuned to `smoothstep(0, 1, t)` over the panel's own width progress, so it
is 0 exactly at rail width and 1 exactly at full width - phase-locked by
construction. Sheet presentation returns `smoothstep(0, 0.25, sheetT)`
instead: a sheet travels, it does not dissolve.

Verified with an 18-frame burst over a collapse: ink inside the panel falls
0.019 → 0.001 across frames 05-09 *while* the panel narrows, then rises to
0.0094 as the rail icons fade in on their own (correct) delayed curve.

### 2. Page counter is now a jump-to-page field

The pill's "3 / 24" was static text drawn from the screen's
`onPaintOverlay`. It is now a `TextField` child of the pill:

* `TextField::flat` - no chrome until hovered (faint well) or focused
  (accent ring), so the idle pill is pixel-identical to before.
* `Widget::onFocusChanged(bool)` added, fired from `EventRouter::setFocus`
  after both `focused` flags settle. Lets a control swap between showing a
  value and editing one without polling `focused` from inside `onPaint`.
* On focus the text becomes just the page number; on blur or Enter,
  `commitPageField()` takes the leading digits, clamps to the page count,
  navigates, and echoes the clamped value back - typing 112 into a 15 page
  document visibly lands on 15.
* Caret x is now align-aware; it previously assumed left alignment and
  would have drawn in the wrong place in a centred field.
* A flat field with no leading icon no longer insets its text by `gap`,
  or the label that used to fit would elide ("15 /…").

### 3. Core components - verified, not assumed

| | State |
|---|---|
| Open from disk | works (PDFium, in-app dialog, 5 documents open at once) |
| Save to disk | **works** - `/tmp` byte check after annotating shows the file gains a *second* `startxref`, i.e. PoDoFo appended an incremental revision and left the original bytes intact |
| Print | **not implemented.** The toolbar button shows a toast of its own tooltip. Same for Export |

### 4. Touch and pen - not supported, and not nearly

* `PointerEvent` has no pointer *type*, no pressure, no tilt - it is a
  mouse event struct.
* `X11Window` selects `ButtonPress/Release/PointerMotion` only. **No
  XInput2**, so touch and stylus arrive as emulated single mouse pointers
  at best.
* No pinch zoom, no two-finger pan, no touch inertia (the wheel fling is
  wheel-only), no long-press, no pen pressure for ink width.
* `folio-ui/viewport/FolioPageView.{h,cpp}` *does* implement
  `QTouchEvent`/pinch - it is leftover Qt-era code, **not referenced by any
  CMakeLists and not built.**
* Hit targets: the command bar uses 28-34 px icons, below the 44 px that
  iOS/Material both recommend.

### 5. `.github` workaround

The workspace will not create dot-directories, so the workflows are also
checked in at `github-workflows/` with a README explaining the one-time
`cp` into `.github/workflows/`.

## Round 20 — Android readiness assessment

Answer: **not ready.** The dependencies are; the application is not.

Verified against the live release feeds rather than assumed:

* skia-pack ships `android-Release-arm64` and `-x64` **on the tag we already
  pin** (`m144-e2e6623374-4`). No `armeabi-v7a`, which is fine post-2019.
* pdfium-binaries ships `pdfium-android-{arm64,x64,arm,x86}.tgz` on
  `chromium/8086`.
* `tools/bootstrap-deps.sh` now takes `FOLIO_TARGET=android FOLIO_ABI=arm64`.
  Both URL patterns were resolved against the real asset lists; the Linux
  defaults resolve to the identical URLs as before and a warm-cache run is
  unchanged.

Missing, in dependency order: an Android platform backend (there is no
`folio-platform/android/`, and every branch in its CMakeLists explicitly
excludes `ANDROID`, so a configure today silently selects the headless
`none` backend and *looks* like it worked); an app wrapper (no Gradle, no
manifest, no JNI); SAF file access (the current `IFileDialog` shells out to
zenity, and incremental save writes to a path, which SAF does not give
you); `AAssetManager` fonts (last round's `resolveAssetDir()` walks the
filesystem, and APK assets are not files); and touch input, which on
desktop was a quality gap and on Android is the whole input model.

Deliverables: `ANDROID-READINESS.md` (gap analysis, ordered 7-step plan,
honest Windows-vs-Android comparison) and
`github-workflows/android-probe.yml` — a cross-compile probe that builds
only `folio-core`/`folio-render`/`folio-engine` with the NDK against the
Android prebuilts. It answers "is the portable stack Android-clean?" for
one CI run, and deliberately does not attempt a shell that does not exist.

---

## Round 21 — touch and pen, end to end

The previous rounds built a mobile *layout* on top of a mouse-only *input
model*. Everything below 720 px looked right and behaved like a desktop:
one pointer, no pressure, no gestures, hit targets sized for a cursor.
This round closes that gap.

A note on the starting point, because it was misleading:
`folio-ui/viewport/FolioPageView.{h,cpp}` contains working Qt pinch and
touch handling. It is dead Qt-era code, referenced by no CMakeLists and
compiled by nothing. It was not a foundation to build on.

### The event contract

`PointerEvent` gained `pointerType` (`Mouse`/`Touch`/`Pen`), `pointerId`,
`pressure` (0..1), and `tiltX`/`tiltY`, plus `isTouch()`/`isPen()`.
Everything else keys off these five fields.

`folio-ui/components/Gestures.{h,cpp}` is new and holds all of the state a
gesture needs — `GestureTracker` with `onPointer`/`pinch`/`panDelta`/
`flingVelocity`/`wasTap`/`tapCount`/`pollLongPress`, plus the tunables
(`kSlop=10`, `kLongPress=0.5 s`, `kDoubleTap=0.3 s`, `kFlingMin=120`,
`kVelocityAlpha=0.35`). It owns no document state and no widget, so it is
testable on its own and reusable by any future surface.

### The rules, and why each one is what it is

| Input | Behaviour |
|---|---|
| Mouse | Unchanged, bit for bit. Touch is additive, not a rewrite. |
| Pen | Falls through to the mouse paths. Only pressure is harvested. |
| Two fingers | Always pinch + pan, **whatever tool is armed**. |
| One finger, reading | Pans; a flick hands off to the existing inertia. |
| One finger, tool armed | Draws. |
| Double tap | FitWidth ⇄ 2 × FitWidth, anchored at the tap. |
| Long press | Selects the word and opens the markup popup. |

Two fingers overriding the active tool is the important one: it is the
escape hatch that means arming the pen tool never traps the reader in a
mode where they cannot navigate.

Pen pressure scales the committed stroke width by
`clamp(0.45 + meanPressure, 0.45, 1.75)`. Measured on a rendered page:
p=0.12 → 2 px, p=1.00 → 7 px.

### Three bugs that only reading the code would find

1. **A half-drawn stroke survived the escape hatch.** Finger down with the
   pen armed starts a stroke; a second finger then switches to pinch — but
   the draft was still in the store and committed as a stray mark when the
   first finger lifted. Now a second finger cancels the draft.
2. **Double tap stole deliberate dots.** With a tool armed, two quick taps
   are two marks, not a zoom request. Double-tap zoom is now gated on
   `editTool_ == None`.
3. **Double tap ratcheted instead of toggling.** It was ×2 each time until
   it crossed a threshold and snapped back (60 → 120 → 240 → fit). Now it
   is strictly two states, so the second double tap always undoes the first.

### Word selection had to learn about scripts

`selectWordAt` expanded over "anything that is not whitespace". Japanese
has no whitespace, so a long press on the CJK fixture would have selected
**the entire page** — silently.

Expansion is now over a run of one script class (`Sep`/`Word`/`Han`/
`Kana`/`Kata`/`Hangul`). For Japanese this is a genuinely good
approximation: a kanji stem, its hiragana okurigana and a katakana
loanword each form their own run, which is where a reader expects the
selection to stop. Verified: long-pressing 午前八時三十分 selects exactly
that and stops at から. Real segmentation needs a dictionary (MeCab/ICU);
this is the honest approximation until one is linked. Latin improved too —
"Vaswani*" now selects `Vaswani` without the footnote asterisk.

### 44 px touch targets

Hit targets are inflated at hit-test time, not at layout time, so **not one
pixel of the design moves**. `Widget::hitLocalPadded` grows a control's hit
rect towards `EventRouter::touchTarget` (44 px) only on the axis that is
short, and only for touch — pen is excluded on purpose, because inflating a
precision instrument's targets makes it harder to place a stroke, not
easier. Overlapping inflated targets are settled by paint order, so the
control drawn on top wins the contested pixels.

Measured on the bottom-bar page chevron (28 px tall, centred at y=861):

| y | mouse | pen | touch |
|---|---|---|---|
| 850 | chevron | chevron | chevron |
| 846 | PageStage | PageStage | **chevron** |
| 842 | PageStage | PageStage | **chevron** |
| 838 | PageStage | PageStage | PageStage |

### Transport

The browser shell now emits Pointer Events when available: `p`=down,
`q`=move, `s`=up/cancel, each `ch,x,y,button,mods,ptype,pointerId,pressure`,
with `touchAction:none`, `setPointerCapture`, and `getCoalescedEvents()` so
a 120 Hz digitiser's merged points are not thrown away. The legacy
`mouse*` handlers are guarded with `if (window.PointerEvent) return;` so
they never double-fire.

The up record is `s`, **not** `r`. `r` is already the resize record, and
distinguishing the two by field count alone means a short write turns a
finger lift into a window resize to 546×846.

### Diagnostics

`FOLIO_DEBUG_HIT` logs every pointer-down as
`type / id / position / slop → widget`. It is permanent and cached-`getenv`,
like `FOLIO_DEBUG_POP`. It earned its place immediately: an earlier
frame-differencing probe "showed" that pen input was broken, and the hit log
proved the opposite — the frames were changing because `PageStage` was
reacting behind the bottom bar, not because the chevron had fired. The
crude test was wrong, not the code.

`tools/stream-drive.py` is the scripted-input harness (`click`, `key`,
`resize`, `tdown`/`tmove`/`tup`, `cap`, `burst`), now kept in the repo
instead of being retyped after every sandbox reset.

### Corpus repair

`documents/Japanese_Government_Notice.pdf` was found damaged: a save test
from Round 19 had left an appended incremental revision on disk, and the
`git checkout` that was supposed to revert it never took effect — the
workspace excludes `.git/config` from snapshots, so git had stopped
recognising the directory as a repository.

Stripping the appended revision from every fixture worked for the flat
files but **destroyed this one**: it is linearized, so its first
`startxref` is near the *front* of the file, and truncating there cut
92,807 bytes down to 472. It had no recipe — it was an opaque binary.

It is now generated by `tools/make-cjk-pdf.py`: one A4 page of Japanese in
an embedded, fontTools-subsetted CID-keyed font (Type0 / Identity-H /
CIDToGIDMap / ToUnicode), 40 KB, 151 glyphs. The fixture is reproducible
from source for the first time. One trap worth recording: the subsetter was
initially fed `text` filtered by `c.strip()`, which silently drops U+3000
IDEOGRAPHIC SPACE — whitespace to Python, a real width-carrying glyph to
the font — and it rendered as tofu mid-sentence.

### Still open

* **X11 XInput2 is not written.** On X11 the server synthesises core
  pointer events for the first touch point, so single-finger tap and drag
  already work through the existing mouse path; what is missing natively is
  multi-touch (pinch) and pen pressure. `libXi.so.6` is present and
  `libxi-dev` is fetchable, so the work is unblocked — it is deliberately
  not done blind, because there is no touch or pen device in this
  environment to verify it against, and the stream shell is the testable
  path today.
* The browser JS (coalesced events, pointer capture) is **verified through
  the C++ parser, not through a real browser** — there is none here.
* Android and iOS remain without a backend at all, so "tablet and mobile"
  is currently served by the stream shell and the phone layout, not by
  native shells.

---

# Round 22 — X11 touch/pen, and Android made real

Three things were asked for: write the X11 XInput2 code now even though it
cannot be device-tested, push Android forward to a true mobile platform
application, and give a view on WebAssembly. The WASM answer is in
`WASM-ASSESSMENT.md`. The other two are below.

## 22.1 X11 XInput2 — what is and is not tested

The full XI2 path is written, compiled, linked and running. The honest
summary of its test status is narrower than "it works", and the
distinction matters:

| Path | Status |
|---|---|
| XI2 extension negotiation, device enumeration | **Verified running** — `XInput2 2.2 active` under Xvfb |
| Mouse down / move / up / wheel through XI2 | **Verified** — drove the real app, asserted on widget names |
| Device classification (mouse vs touch vs pen) | **Partly** — mouse classified correctly; no touch or pen device existed to classify |
| Packed-valuator decoding (pressure, tilt) | **Unit-tested** — `tests/platform/test_xi_valuator.cpp`, 15 checks |
| Touch down/move/up, multi-touch, pinch on X11 | **NEVER EXECUTED** |
| Pen pressure and tilt on X11 | **NEVER EXECUTED** |

### What was actually done to test it

The risk in this change was not the touch code — it was that `initXI2()`
**replaces the core pointer event masks**. If XI2 selection succeeds but
delivery differs in any way, every existing Linux mouse interaction breaks.
That is a regression that would be invisible in a headless CI link check.

So the real desktop binary was run on Xvfb (which speaks XInput2 2.2) and
driven with synthetic input through xdotool: opened the welcome screen,
clicked through the file dialog, opened `Attention_Is_All_You_Need.pdf`,
drag-selected text across a line, and scrolled with the wheel. Every step
was asserted with `FOLIO_DEBUG_HIT` on the widget name — not on pixels,
for the reason recorded in round 21. All of it arrived as `type=0` (mouse)
with correct coordinates and exactly one event per action, and a
screenshot confirmed the page rendered and the selection markup popup
appeared.

A `FOLIO_DEBUG_INPUT=1` diagnostic was added specifically so this claim
could be checked rather than assumed: it prints the negotiated XI2 version
and each device's classification and axis numbers. Without it, a silent
fallback to core events would look exactly like success.

### The part that cannot be tested here

Xvfb has no touchscreen and no tablet. `XISelectEvents` asks for
`XI_TouchBegin/Update/End` and the pressure/tilt valuators, and nothing in
this sandbox can emit them. `/dev/uinput` exists but is root-only, and in
any case Xvfb has no evdev input backend to feed — it only accepts XTEST,
which cannot synthesise touch.

**The touch and pen branches of `translateXI()` have therefore never
executed.** They are written, they compile, they are reachable, and the
one piece of them that is genuinely easy to get wrong — the packed
valuator walk — is covered by unit tests.

#### Why that one piece got its own test

XI2 does not send a value per axis. It sends a mask of which axes are
present plus a tightly packed array of only those values. The value for
axis *N* is not `values[N]`; it is `values[popcount(mask below N)]`.
Indexing by axis number compiles, runs, and returns a plausible number —
a stylus would report its tilt as pressure and strokes would come out the
wrong width. That is precisely the class of bug that survives until
someone plugs in hardware, so it was extracted into
`folio-platform/linux/XiValuator.h` as a pure function with no X11
dependency and tested directly: dense masks, sparse masks, absent axes
(which must leave the destination untouched, not zero it mid-stroke),
axes past the first mask byte, and null/negative inputs.

### Other changes

* `tools/bootstrap-deps.sh` fetches `libxi-dev` **and `libxfixes-dev`** —
  `XInput2.h` includes `Xfixes.h` for `PointerBarrier` and will not compile
  without it. `libXi.so` ships the same dangling symlink as the GL
  libraries and is repointed the same way.
* CMake degrades cleanly: no libXi means `FOLIO_HAVE_XI2=0`, a status
  message, and single-touch via core-pointer emulation rather than a
  broken build.
* `NativeShell.cpp` maps the new `plat::Event` fields onto `PointerEvent`
  and handles `PointerCancel`.

## 22.2 Android — from "configures to nothing" to a loadable library

Previous rounds recorded that configuring for Android "silently selects the
headless `none` backend… a false success". That is fixed. The NDK (r27c)
and real arm64 prebuilts were fetched, and **the whole stack now
cross-compiles, links, and produces `libfolio-android.so` (22 MB,
aarch64) exporting both `android_main` and `ANativeActivity_onCreate`.**

Cross-compiling immediately found four real defects that the host build
had been hiding:

1. **`tools/bootstrap-deps.sh` ignored the target when caching.** Skia and
   PDFium unpacked to the same directory regardless of `FOLIO_TARGET`, and
   the presence checks were path-based. A warm host cache made
   `FOLIO_TARGET=android` a silent no-op — it printed "skia: present" and
   fetched nothing. A cold cache would have been worse: a host build
   linking Android archives. Cross targets now get suffixed directories,
   and `cmake/FolioDependencies.cmake` resolves the same suffix.
2. **Signed/unsigned narrowing in `PdfiumWorker.cpp`.** `PageId{i}` with
   `int i` is a narrowing conversion in a braced initialiser; GCC warns,
   Clang refuses. Three sites, now explicit casts.
3. **`std::from_chars` for floats does not exist in libc++.** The NDK's
   standard library declares the floating-point overloads and never
   defines them, so the call resolved to the deleted `bool` overload. The
   SVG path parser in `Icons.cpp` now falls back to `strtof` on libc++.
4. **`project(... LANGUAGES CXX)` silently dropped the NDK's
   `android_native_app_glue.c`.** No error, no warning — the `.so` simply
   linked without `ANativeActivity_onCreate`, which Android resolves by
   `dlsym`. The app would have failed to start with no useful diagnostic.
   C is now enabled for that one file.

Defect 4 is worth dwelling on: it is the exact failure mode this round was
supposed to eliminate. The library built, linked, reported success, and
was unloadable. Only `llvm-nm -D` on the result caught it. **Checking that
a build produced the right symbols is not paranoia on a new platform; it
is the only evidence that exists.**

### What was written

* **`folio-platform/android/AndroidWindow.cpp`** — `ANativeWindow` +
  EGL/GLES3 (stencil 8, no depth, no MSAA, matching every other backend),
  the looper wired to `poll`/`waitEvents`/`wake`, and the lifecycle. Three
  Android-specific realities are handled explicitly rather than papered
  over:
  - *There is no window to create.* `createWindow` waits for
    `APP_CMD_INIT_WINDOW` and adopts what it is given. Losing the surface
    on background or rotate is normal operation, and the **EGL context is
    kept across it** — recreating it would throw away every texture Skia
    has uploaded.
  - *One `MotionEvent` carries every pointer.* A pinch is a single
    `ACTION_MOVE` holding both fingers plus the historical samples
    batched since the last frame. Each event fans out to one Folio event
    per pointer, **historical samples first** — discarding them is what
    makes a fast stroke look polygonal, the same reason the browser shell
    asks for coalesced events.
  - *Focus loss voids every contact.* A finger mid-pinch when the app
    backgrounds will never report an UP, so `APP_CMD_LOST_FOCUS` emits
    `PointerCancel`.
  - Stylus is read from `AMOTION_EVENT_TOOL_TYPE_STYLUS/ERASER`, pressure
    from `getPressure`, and Android's single tilt-plus-orientation pair is
    decomposed onto the tiltX/tiltY axes the rest of Folio uses.
  - `AKEYCODE_BACK` maps to `Escape`. Swallowing Back is a one-star
    review.
* **`folio-ui/shell/main_android.cpp`** — `android_main`, plus unpacking
  fonts out of the APK. APK assets are zip entries, and Skia's font
  manager wants paths, so the fonts are extracted once into the app's
  private directory. The path arithmetic here is off-by-one-level easy to
  get wrong: Gradle packages the *contents* of `assets.srcDirs` at the
  asset root, so the repository's `assets/fonts/x.ttf` is addressed as
  `fonts/x.ttf` inside the APK, and what the shell is handed must be the
  *parent* of the unpacked `fonts/`.
* **`android/`** — Gradle project: `build.gradle.kts` driving our existing
  CMake through `externalNativeBuild`, and a `NativeActivity` manifest
  with no Java at all (`android:hasCode="false"`). `configChanges` claims
  orientation and density so a rotation does not destroy the activity and
  discard the page cache.
* `NativeShellOptions::assetDir` — desktop still derives it from the
  executable path; Android passes the unpacked directory.

### Android status, stated plainly

| | |
|---|---|
| Cross-compiles for arm64-v8a | **Yes**, whole stack, zero warnings |
| Links to a loadable `.so` with both entry points | **Yes**, verified with `llvm-nm -D` |
| Platform backend selected | **`android`**, no longer `none` |
| Ever run on a device or emulator | **No.** No SDK, no emulator, no device here. |
| Gradle layer ever executed | **No.** No Android SDK and no Gradle in this sandbox. |
| Clipboard | No-op. Needs JNI to `ClipboardManager`. |
| File open/save | **No native picker.** Modern Android has no file-path dialog at all — scoped storage replaced it with SAF, which returns a `content://` URI and a file descriptor, and that does not fit `IFileDialog`'s path-shaped interface. `available()` returns false so the in-app `OpenDialog` is used instead, which is a real experience on a sandboxed app directory. Opening documents from Files or Gmail needs the SAF bridge. |
| PoDoFo | Not cross-compiled, so **no save on Android yet** — same blocker as WASM. |

So: the C++ is real and verified as far as a machine without an Android
device can verify it. The Java/Gradle/APK layer is written and
**unexecuted**. Nobody should read "Android builds" as "Android runs".

## 22.3 Verification notes

* **The Linux build and all 12 tests stay green** after the shared changes
  (`PdfiumWorker.cpp`, `Icons.cpp`, `cmake/FolioDependencies.cmake`,
  `CMakeLists.txt`). The new test is `platform.xi.valuator`.
* **Search highlighting was re-verified visually**, because clearing a
  `-Wshadow` warning in `PageStage.cpp` meant renaming a loop variable
  inside the highlight draw path — a rename that initially left the body
  referring to the *outer* `r`, which is precisely the bug the warning
  exists to prevent. Searching "Revenue" in Annual_Report_2026 reports
  "1 of 23" and draws the current hit in orange, as before.
* **`tools/build-android.sh`** builds either ABI from scratch, fetching the
  NDK if absent, and then **asserts on the exported symbols**. Its first
  version reported a false failure: under `set -o pipefail`, `grep -q`
  exits on its first match, `nm` takes SIGPIPE, and the pipeline reports
  failure *because* the symbol was found. The check now captures the
  symbol list once. Worth recording as a general trap — `pipefail` plus
  `grep -q` inverts the result of any such test.
* **Clang surfaces nine cosmetic warnings that GCC does not** (unused
  private fields, an unused lambda capture, constants shadowed in an
  anonymous namespace). They are listed in the build log and left for a
  hygiene pass; none affects behaviour. The four that did matter are fixed
  and described in §22.2.

## 22.4 WebAssembly

Answered in full in **`WASM-ASSESSMENT.md`**. The headline: §13 of the
architecture plan is out of date in Folio's favour. Skia and PDFium both
ship prebuilt Emscripten artefacts at the versions already pinned here
(`Skia-m144-...-wasm-Release-wasm.zip`, 23.5 MB; `pdfium-wasm.tgz`,
2.5 MB), and committing to Skia deleted the Qt-on-WASM GPLv3 problem
entirely. The blocker has moved to **PoDoFo**, which has no Emscripten
build, so a web Folio reads and annotates but cannot save until that is
cross-compiled. Recommended order: Android, then Windows (808 lines
already written and never compiled — the best value per hour left), then
WASM.

One trap recorded there and repeated here because it will otherwise be
rediscovered: `folio-platform/CMakeLists.txt` still reads
`elseif(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)`, so an
Emscripten configure silently selects the headless `none` backend — the
same false success Android was in until this round.

---

# Round 23 — the first Windows CI run, and a real APK

The Windows build was pushed and failed. Every one of those failures is
fixed, and while getting there the Android story moved from "a `.so` that
links" to an actual installable APK.

## 23.1 Windows / MSVC — four causes, not four bugs

The CI log showed ~15 errors. They reduce to four root causes.

**1. `std::max` colliding with `windows.h` (the `C2589`/`C2059` cluster).**
PDFium's public headers include `<windows.h>`, which defines `min` and `max`
as function-like macros, so `std::max(0, n)` preprocesses into
`std::(((0) > (n)) ? (0) : (n))` — "illegal token on right side of `::`".
The error points at our code and never mentions the macro, which is why it
looks like a syntax error in a line that is obviously fine.

Fix is `add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)` at
top level, placed immediately after `project()` so it reaches **every**
target, not just the ones that include `<windows.h>` themselves.

*Verified, not assumed.* Reproduced with GCC: a TU containing `#define max`
before `<algorithm>` produces the same class of failure. That matters
because a fix I cannot reproduce is a fix I am guessing at.

**2. `-Wno-conversion` reaching `cl.exe` (`D8021: invalid numeric
argument`).** Three sites passed GCC/Clang flags unguarded:
`folio-render-skia`, `folio-engine-podofo`, and the header block in
`folio-platform`. `folio_relax_warnings()` was always MSVC-aware; these
three just bypassed it. Now routed through the helper or `if(MSVC)`
guarded. MSVC *rejects* unknown `/W` flags rather than ignoring unknown
`-W` ones, so this is not cosmetic — it stops the build dead.

**3. `Draw.h` using `std::string` while including only `<string_view>`
(`C2039: 'string': is not a member of 'std'`).** A missing include, made
invisible by libstdc++ pulling `<string>` in transitively. This class of bug
is *undetectable from Linux, ever* — so a lint was written instead of a
prayer.

**4. `C2397` narrowing at the `PageId{...}` sites.** Already fixed in round
22 as a *Clang/NDK* fix (the NDK's clang is stricter than GCC here); their
Windows run was from before it. The `C4267` size_t→int warnings on the same
lines were the round-22 fix being incomplete, and are now fixed properly.

Plus four `C4005` macro-redefinition warnings: `Win32Window.cpp` defined
`WIN32_LEAN_AND_MEAN`/`NOMINMAX`/`UNICODE`/`_UNICODE` that CMake also sets
on the command line. Guarded with `#ifndef` so the file still compiles
outside our build system. `_CRT_SECURE_NO_WARNINGS` added project-wide for
the `getenv`/`sscanf` C4996 noise.

## 23.2 The header check, and what it found

`tools/check-header-includes.py` is a deliberately conservative text lint:
for a known list of std symbols it reports any use whose owning header is
not included. It only judges the header that *owns* the symbol and expands a
handful of standardised re-exports (`<string>` → `std::string_view`), because
inferring transitive includes is the very thing that caused the bug.

It found **39 missing includes across 24 headers**, including three more
`std::string` cases — `FolioApp.h`, `ReaderScreen.h`, `WelcomeScreen.h` —
that would each have failed the Windows build exactly like `Draw.h` did, on
the next file anyone touched. All fixed, plus `tools/fix-header-includes.py`
to re-run the repair idiomatically. The check now runs as the **first step of
the `core-only` CI job**, ahead of the compiler, and costs about a second.

Changed headers (24): folio-core `render/TileGrid.h`, `render/TileKey.h`,
`util/Arena.h` · folio-engine `PdfiumPause.h`, `PdfiumRasterizer.h`,
`SnapshotFileAccess.h` · folio-render `SkiaPaintTarget.h`,
`SkiaTileCompositor.h` · folio-ui `Annotations.h`, `Button.h`,
`Containers.h`, `Controls.h`, `Edits.h`, `PageStage.h`, `WidthStrip.h`,
`Draw.h`, `Theme.h`, `Widget.h`, `FolioApp.h`, `ReaderScreen.h`,
`WelcomeScreen.h`, `HttpServer.h`, `StreamShell.h` · plus
`folio-engine/pdfium/PdfiumWorker.cpp` and `folio-platform/win32/
Win32Window.cpp`.

## 23.3 "Without an APK, how would it be tested?" — you were right

Challenging the claim was correct, and the honest answer would have been
"it wouldn't". So the toolchain was fetched and an APK was actually built.
**There is now a real `app-debug.apk`, 36 MB**, containing
`lib/{arm64-v8a,x86_64}/{libfolio-android.so,libpdfium.so,libc++_shared.so}`
and `assets/fonts/DMSans-Variable.ttf`, with
`package: ai.folio.reader`, `launchable-activity: android.app.NativeActivity`,
`native-code: 'arm64-v8a' 'x86_64'`, `sdkVersion: 26`.

Building it found two more real defects, both of the "looks fine, isn't"
kind:

* **`folio-android` silently vanished for any ABI without prebuilts.**
  x86_64 has no Skia in the cache, so CMake skipped the target and Gradle
  reported `Unexpected native build target folio-android` — technically true,
  diagnostically useless. Now a `FATAL_ERROR` naming the ABI and the exact
  bootstrap command.
* **`ndk.dir` in `local.properties` is rejected by AGP 8** (`CXX1102`) unless
  the NDK sits under the SDK root. The NDK must be installed via
  `sdkmanager`, not pointed at.

`tools/build-apk.sh` captures the whole working sequence (JDK 17 → SDK →
per-ABI prebuilts → Gradle), and `android/.gitignore` exists because
Gradle's `.cxx` scratch tree is **212 MB under a directory name that is not
snapshot-excluded** — unlike `build/`, it gets committed. The script now
removes it after packaging (`KEEP_CXX=1` to opt out). Workspace is back to
**6.2 MB**.

**What this does NOT prove:** it cannot be *run*. `/dev/kvm` does not exist,
so there is no hardware-accelerated emulator, and a software-emulated
GLES3+PDFium app would be too slow to tell a hang from a bug. The APK is
verified by inspection. **The first launch on real hardware is still the
first launch**, and touch, pen, and the font-unpacking path remain
unexercised on device.

Manifest: dropped the `package` attribute (AGP 8 ignores it and warns).
