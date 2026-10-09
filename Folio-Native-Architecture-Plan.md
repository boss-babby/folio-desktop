# Folio — Native C++ Implementation Plan
### PDFium + PoDoFo · Windows · macOS · Linux · iOS · Android · WebAssembly
**Target:** pixel-identical UI on all six platforms, 60 fps floor (120 fps where the display allows), GPU-composited.

---

## Table of contents

1. [Executive summary & the one decision that matters](#1-executive-summary)
2. [UI stack: comparison and recommendation](#2-ui-stack-decision)
3. [What the prototype actually specifies](#3-what-the-prototype-specifies)
4. [System architecture](#4-system-architecture)
5. [The dual-engine problem: PDFium + PoDoFo under one roof](#5-dual-engine)
6. [Responsibility split: who does what](#6-responsibility-split)
7. [Threading model and the frame budget](#7-threading-and-frame-budget)
8. [The render pipeline: tiles, LOD, caches, GPU upload](#8-render-pipeline)
9. [Layer model: raster · overlay · chrome](#9-layer-model)
10. [Animation system — mapping every motion in the demo](#10-animation-system)
11. [Design tokens and the component kit](#11-design-tokens)
12. [Platform shells and adaptive layout](#12-platform-shells)
13. [WebAssembly: the hard target](#13-webassembly)
14. [Performance engineering playbook](#14-performance)
15. [Build system, dependencies, CI](#15-build-and-ci)
16. [Testing — including mechanical enforcement of "same UI everywhere"](#16-testing)
17. [Security](#17-security)
18. [Risk register](#18-risks)
19. [Roadmap and effort](#19-roadmap)
20. [Appendix A — licensing](#appendix-a-licensing)
21. [Appendix B — engine API cheat-sheet](#appendix-b-api)
22. [Appendix C — the Skia route: what varies per platform](#appendix-c-skia)
23. [Appendix D — what was built and measured on Linux](#appendix-d-validation)

---

<a name="1-executive-summary"></a>
## 1. Executive summary & the one decision that matters

Folio is three products stacked on top of each other:

- a **document renderer** (hard, performance-critical, solved by PDFium),
- a **document editor** (hard, correctness-critical, solved by PoDoFo),
- a **highly animated application shell** (the thing your prototype actually specifies, and the thing that will consume most of the engineering budget).

The prototype is not a mood board. It is a precise specification: 30+ distinct named motions, two complete colour themes, a 5-mode tool model, a circular-reveal panel system, and a document viewport that must stay interactive during edits. Reproducing it natively on six platforms *without drift* is the central engineering challenge — not the PDF part.

### The recommendation

| Layer | Choice |
|---|---|
| UI toolkit | **Qt 6.8 LTS+ / Qt Quick (QML) with a 100% custom component kit** — no Qt Quick Controls native styles |
| Document viewport | **Custom `QQuickItem` + `QSGNode` tile renderer** (not an `Image`, not a `QQuickPaintedItem`) |
| Rasterisation / text / forms | **PDFium** (BSD-3) |
| Document model / save / structure | **PoDoFo 1.0** (LGPL-2.0+ **or MPL-2.0**) |
| Core application code | **Portable C++20, zero Qt types** behind an interface boundary |
| Graphics backends | Qt RHI → D3D12/Vulkan/Metal/OpenGL/WebGL |

**The critical caveat, surfaced by research:** Qt requires a **commercial licence for two of your six targets** if Folio is proprietary.

- **WebAssembly** is licensed **GPLv3 or commercial only** — there is no LGPL option, unlike the rest of Qt Quick.
- **iOS** is **static-link-only** — Qt flatly refuses to build shared libraries for iOS, and the LGPLv3 position on static linking there is contested enough that Qt's own answer is "buy commercial" (§2.1).

Android and the three desktop platforms are fine under LGPL. This is the single largest cost/licence risk in the plan and is why §2 and §4 insist on an architectural escape hatch. Note that **neither PDF engine contributes to this problem**: PDFium is BSD-3, and PoDoFo 1.0 can be taken under MPL-2.0, which permits static linking into proprietary code on any platform. Every licensing decision here is a Qt decision.

### The escape hatch (do this regardless of toolkit)

Define the entire UI as **~26 primitive components driven by a shared `design-tokens.json`**, and keep every line of application logic in portable C++ with no toolkit types. If Qt's licensing becomes a blocker, you re-implement 26 components in Slint — not the application. That turns a catastrophic migration into a ~6-week one. This constraint costs almost nothing to honour on day one and is nearly impossible to retrofit later.

### Honest effort estimate

**16–18 months** to full feature parity across six platforms with a team of five (2 UI, 2 core/engine, 1 platform/build). A credible Read-mode-only desktop beta lands at month 5. Anyone promising materially less has not accounted for Edit mode or iOS.

---

<a name="2-ui-stack-decision"></a>
## 2. UI stack: comparison and recommendation

You asked me to compare and commit. Here is the comparison against *your* constraints, not generic ones.

### Scoring against the brief

| | Qt 6 Quick | Slint | Custom Skia engine | Embedded webview |
|---|---|---|---|---|
| Win / macOS / Linux | Excellent | Good | Excellent | Good |
| iOS | Excellent | Emerging | Good (you own it) | Poor (no CEF; WKWebView ≠ CEF) |
| Android | Excellent | Good, newer | Good (you own it) | Poor |
| **WebAssembly** | Works; **GPLv3/commercial only** | Supported, historically demo-grade | Excellent (CanvasKit) | N/A |
| Pixel-identical across OS | Yes, if you ban native styles | Yes | Guaranteed by construction | Yes |
| Animation expressiveness vs. demo | Excellent (`Behavior`, `Animator`, `ShaderEffect`) | Excellent — the DSL is closer to this CSS than QML is | Total control | Native (it *is* CSS) |
| 60 fps with GPU scene graph | Yes — threaded render loop, RHI | Yes — designed for it | Yes | Fragile under load |
| Text shaping, IME, CJK/RTL | Included, battle-tested | Good, less proven | **You must build it** | Included |
| Accessibility | UIA / NSAccessibility / AT-SPI / Android a11y | Partial | **You must build it** | Included |
| Custom GPU viewport integration | `QQuickItem` + `QSGNode` — first class | Possible, thinner | Native | Painful (IPC per frame) |
| Ecosystem: printing, file dialogs, DnD | Excellent | Thin | None | Partial |
| Licence for proprietary product | LGPLv3 desktop/mobile; **GPL/commercial for WASM** | **Royalty-free for proprietary desktop + mobile + web** | BSD (Skia) — clean | Ultralight commercial; CEF fine |
| Time to parity | Baseline | +15% | **+120–200%** | −40% but fails platforms |

### Verdict

**Primary: Qt 6 Quick.** It is the only option that is simultaneously mature on all six targets, ships the invisible 40% (IME, accessibility, HiDPI, printing, native file dialogs, drag-and-drop, clipboard), and gives first-class integration for the custom GPU viewport you must write anyway. Qt Quick's scene graph already does what you'd otherwise hand-roll: retained GPU geometry, automatic batching, a dedicated render thread, and `Animator` types that run animations *on the render thread* so they survive a stalled UI thread. That last feature is the difference between "usually 60 fps" and "60 fps while a 400-page PDF is loading".

**Runner-up: Slint.** Choose it if (a) the commercial Qt licence is unacceptable, and (b) web is a first-class target. Slint's `.slint` language maps to your CSS more directly than QML does — states, transitions and easings are near one-to-one with the prototype — and the royalty-free licence explicitly covers proprietary desktop, mobile *and* web at zero cost. The costs are a thinner ecosystem (you will write your own print and file-dialog plumbing), less mature iOS, and web support that has historically been positioned as demo-grade. Re-evaluate at the Phase 0 spike.

**Rejected: embedded webview** (CEF/Ultralight). It would preserve your design for free, but it cannot ship to iOS/Android/WASM coherently, it puts a browser between you and the GPU, and per-frame IPC for a scrolling document viewport is exactly the wrong shape. Mentioned only because it is the obvious shortcut and should be explicitly ruled out in writing.

**Rejected as primary: custom Skia engine.** It is the only approach that makes pixel-identity a theorem rather than a test result, and for a 10-year product platform it is defensible. But you would be building text shaping, bidi, IME, accessibility, focus management and six platform shells before writing a single line of PDF code. Keep it in the drawer.

### 2.1 iOS linking, binary size and RAM — the three standard objections

These come up immediately and deserve measured answers rather than hand-waving.

#### Is Qt dynamically linkable on iOS? No — but not because of Apple.

iOS has permitted **embedded dynamic frameworks since iOS 8 (2014)**; the "Apple forbids dylibs" rule is long obsolete. The blocker is Qt itself, which refuses to configure a shared build for iOS:

```
CMake Error at qtbase/cmake/QtAutoDetect.cmake:14 (message):
  Building Qt for iOS as shared libraries is not supported.
```

Qt's documentation confirms the intent: *"In Qt for iOS, everything is compiled statically and placed into the application bundle."* There is no supported `-shared` path, and Qt reaffirmed the stance as recently as the PySide6-on-iOS work. Two practical consequences beyond licensing:

- **All QML plugins must be linked and registered statically** (`qtquick2plugin`, `qquicklayoutsplugin`, …). This is a build-configuration cost, not a blocker, but it bites the first time you deploy and get a blank screen.
- **QtWebEngine is unavailable** in static builds and on iOS. Irrelevant here — we rejected the webview approach in §2 — but worth knowing.

**LGPL consequence.** Static linking is *not* automatically a licence violation. GNU's own FAQ permits it provided you also ship your application in object form so a user can relink against a modified Qt. That is clean enough on desktop. On iOS it becomes contested, because LGPLv3 additionally requires the user be able to *run* the relinked binary, which pulls in code signing and provisioning. The Qt Company's position is consistently "buy commercial". **Treat iOS as a commercial-licence target.**

#### Does PoDoFo have the same problem? No.

| Library | iOS linking | Licence consequence |
|---|---|---|
| **PDFium** | static, fine | BSD-3-Clause — attribution only |
| **PoDoFo 1.0** | static, fine | **LGPL-2.0+ *or* MPL-2.0 — elect MPL-2.0** |
| **Qt** | static, forced | LGPLv3 contested on iOS → commercial in practice |

Electing **MPL-2.0** for PoDoFo removes the question entirely: MPL is file-level copyleft, so statically linking into proprietary code is explicitly permitted and you only publish modifications to PoDoFo's own files. No object-file deposit, no grey area.

Two conditions: use **PoDoFo 1.0 or later** (0.10.x is LGPL-only — the MPL option arrived with the 1.0 relicensing), and do not ship the `podofo*` command-line tools, which remain GPL-2.0+.

> **Qt is the only library in the entire stack with an iOS licensing problem.**

#### Binary size — iOS

| Component — iOS arm64, static, stripped | Size |
|---|---|
| Qt Quick runtime (Core/Gui/Qml/Quick/Shapes) | ~14–22 MB · **~8–12 MB** with Qt's size-minimised configure |
| *Slint equivalent* | *~3–6 MB* |
| PDFium, `pdf_enable_xfa=false pdf_enable_v8=false` | ~6–9 MB |
| PDFium **with** XFA + V8 | ~25–35 MB — never ship |
| PoDoFo + freetype + zlib + libpng + libjpeg-turbo + libxml2 | ~4–6 MB |
| OpenSSL — only if signing is enabled | ~2.5–3.5 MB |
| DM Sans ×4 weights, app code, QML, assets | ~3–5 MB |
| **Total `.app`** | **~30–45 MB (Qt) · ~18–28 MB (Slint)** |
| **App Store download** after thinning/compression | **~15–25 MB · ~9–14 MB** |

Calibration matters here: **PDFium costs about as much as Qt does.** Switching toolkit saves ~12–17 MB, not the 3× some people expect. Apple's cellular download limit is 200 MB; 30–45 MB is unremarkable. Qt publishes a size-minimisation guide with measured iOS figures — adopt that configure as the baseline, not the default one.

#### Desktop footprint — what you actually ship

You do **not** ship all of Qt. You ship the modules you link, plus the plugins they load at runtime. `windeployqt` / `macdeployqt` automate this, but both are deliberately conservative — the default output is roughly **double** what Folio needs. Budget time to prune it; it is a half-day of work worth ~35 MB.

**Windows x64, Qt 6.8, Qt Quick, Release, dynamically linked:**

| Item | Size | Ship? |
|---|---|---|
| `Qt6Core.dll` | ~6.5 MB | required |
| `Qt6Gui.dll` | ~8.5 MB | required |
| `Qt6Qml.dll` | ~5.0 MB | required |
| `Qt6Quick.dll` | ~6.0 MB | required |
| `Qt6QmlModels` + `QmlMeta` + `QmlWorkerScript` | ~1.1 MB | required |
| `Qt6QuickLayouts` + `Qt6QuickShapes` | ~0.6 MB | required |
| `Qt6Network.dll` | ~1.6 MB | only if update checks / telemetry |
| `Qt6OpenGL.dll` | ~0.5 MB | required by RHI GL fallback |
| `Qt6Svg.dll` | ~0.4 MB | **omit** — icons come from the SDF atlas (§14.2) |
| `platforms/qwindows.dll` | ~1.6 MB | required |
| `tls/`, `imageformats/`, `iconengines/` | ~1.5 MB | prune to what you use |
| `qml/QtQuick/**` module tree | ~6–10 MB | prune with `--qmldir` |
| **Qt subtotal, pruned** | **~38–43 MB** | |
| `opengl32sw.dll` — Mesa software rasteriser | ~20 MB | **omit** — see below |
| `d3dcompiler_47.dll` | ~4 MB | **omit** if shipping pre-compiled `.qsb` |
| `translations/*.qm` | ~3 MB | omit until you localise |
| `qmltooling/` — QML debugger plugins | ~5 MB | **omit** in release |
| **`windeployqt` raw default** | **~70–80 MB** | |
| **After pruning** | **~38–43 MB** | |

Then the rest of Folio:

| Item | Size |
|---|---|
| `pdfium.dll`, no XFA/V8 | ~10–13 MB |
| PoDoFo + freetype + zlib + libpng + libjpeg-turbo + libxml2 | ~5–7 MB |
| OpenSSL — only if signing | ~5 MB |
| DM Sans ×4 weights | ~0.3 MB |
| `Folio.exe` + QML + shaders + assets | ~4–6 MB |
| MSVC runtime (omit with static CRT) | ~2 MB |
| **Total installed** | **~65–80 MB** |
| **Installer, LZMA2** | **~25–32 MB** |

**The two big omissions explained.** `opengl32sw.dll` is a software OpenGL fallback for machines with no usable GPU driver — at 20 MB it is the single largest file `windeployqt` copies. Folio is a GPU-composited application with a hard 60 fps target; software GL would deliver perhaps 5 fps and a terrible first impression. Ship without it and fail fast with a clear message instead. `d3dcompiler_47.dll` is only needed when shaders are compiled at runtime; with Qt 6 RHI you pre-compile to `.qsb` at build time, so verify with a clean-VM test and then drop it.

**Static linking is *smaller*, and on desktop it's licence-clean.** This is counter-intuitive but important: a statically linked Qt Quick app is typically **~25–35 MB** for the Qt portion versus ~40 MB dynamic, because the linker dead-strips everything you don't call, whereas a DLL must contain everything. Unlike iOS (§2.1), the LGPLv3 static-linking path on desktop is clean — you publish your object files and a relink script, and the user can genuinely rebuild and run the result. No commercial licence needed.

| Desktop deployment | Installed | Installer |
|---|---|---|
| Dynamic, `windeployqt` default | ~100–115 MB | ~40 MB |
| **Dynamic, pruned** | **~65–80 MB** | **~25–32 MB** |
| **Static Qt + object-file deposit** | **~50–65 MB** | **~20–25 MB** |
| Static + size-minimised Qt configure | ~40–50 MB | ~16–20 MB |
| *Slint equivalent, for comparison* | *~30–40 MB* | *~13–17 MB* |

**Per-platform notes.**

- **macOS** — same figures, but a **universal binary doubles everything** (x86_64 + arm64). A universal `.app` lands around 130–160 MB. Consider arm64-only given Apple silicon's share, or ship two builds.
- **Linux** — three options. AppImage bundles Qt (~70–90 MB). Linking against distro Qt ships nothing but invites version skew across Ubuntu/Fedora/Arch. **Flatpak against `org.kde.Platform` is the sweet spot**: the runtime already provides Qt 6, so your Flatpak is only Folio plus the PDF engines, around **20–25 MB**.
- **Windows** — consider MSIX or a delta-updating installer; Qt DLLs rarely change between releases, so users download ~5 MB updates rather than 30 MB.

**Calibration.** 65–80 MB installed is small for a desktop application. Acrobat Reader is ~500 MB, Foxit ~250 MB, VS Code ~350 MB. Qt is not what makes a PDF editor large — the PDF engine and your own assets are, and those are constant across every toolkit choice.



| | Qt Quick | Slint |
|---|---|---|
| Framework + engine + scene graph baseline | ~40–70 MB | ~10–25 MB |
| Tile raster cache (mobile budget, §8.4) | 96 MB | 96 MB |
| GPU texture cache (mobile budget) | 128 MB | 128 MB |
| Document model + text index, 200-page doc | ~25 MB | ~25 MB |
| **Total** | **~290–320 MB** | **~260–275 MB** |

The toolkit is roughly **15% of the memory profile**. The tile cache dominates — and it is a tunable constant in `design-tokens.json`. Dropping the mobile raster budget from 96 MB to 64 MB saves more RAM than switching toolkit, at the cost of some extra re-rasterisation during fast scroll. **Tune the cache, don't pick the framework on RAM grounds.** For scale, iOS jetsams a foreground app somewhere above ~1.4 GB on a 3 GB device.

### 2.2 The decision rule this produces

Qt now effectively requires a commercial licence for **two of six targets**: iOS (forced static linking) and web (GPLv3-only, §Appendix A). That is the mainline case, not an edge case.

> **If there is a commercial Qt budget (~€4–5k/developer/year): Qt stays the recommendation.** The licence costs less than one engineer-month, and rebuilding Qt's printing, native file dialogs, IME and accessibility on Slint would cost far more.
>
> **If that budget is zero: Slint becomes the primary**, accepting thinner iOS/web maturity and more platform plumbing written in-house.

**Do not split the difference.** Qt on desktop plus Slint on mobile/web means two UI implementations, and the design *will* drift — which defeats the entire brief. Pick one and ship it everywhere.

> **Decision gate:** Phase 0 (§19) builds the same tile-viewport spike in Qt Quick *and* Slint, including a WASM build of each and a real iOS device deployment, and **measures** `.app` size and peak RSS rather than trusting the ranges above. Two weeks of work to de-risk an 18-month programme. Do not skip it.

---

<a name="3-what-the-prototype-specifies"></a>
## 3. What the prototype actually specifies

Extracted from the HTML/CSS/JS so the native build has a checklist rather than a vibe.

### 3.1 Screens

| Screen | Contents |
|---|---|
| **Welcome** | Custom title bar, left sidebar (Open / Merge / Images→PDF / Fill & sign / OCR / open-mode segmented control / theme segmented control / Settings), greeting header (time-of-day aware), hero "continue reading" card with progress bar, drop zone, recent-files grid ⇄ list with live search and per-item remove |
| **Document window** | Title bar with animated document tabs, command bar, left panel (Pages/Outline/Notes/Layers), document stage, right panel (mode-dependent), floating bottom pill, find popover, display popover, dynamic menu popover, tooltip layer, toast layer |

### 3.2 The five modes

| Mode | Tools | Right panel |
|---|---|---|
| **Read** | — ("Reading mode keeps the controls out of the way") | Display: page theme (light/sepia/dark), layout (scroll/single/two-up) |
| **Annotate** | Highlight, Underline, Strikethrough, Comment, Draw, Stamp | Highlight colour ×4, pen width, live annotation counts |
| **Edit** | Edit text, Add text, Image, Link, Shape, Redact + Crop, Header/footer, Watermark, Background, Bates, OCR, Compress | Inspector: typeface, size, line height, colour ×4, align, opacity, delete |
| **Organize** | Rotate, Duplicate, Insert blank, Extract, Delete, Merge, Split | Selection count, rotate L/R, duplicate, delete, thumbnail size, select all |
| **Forms** | Text field, Checkbox, Dropdown, Signature field | Field count, highlight-fields toggle, required toggle |

Mode switching is `Ctrl+1..5`. The tool strip cross-fades (`#tools.out`, 160 ms) and the active tool **grows an inline text label** (`max-width: 0 → 130px`) — a small detail that is easy to miss and very visible when absent.

### 3.3 Behaviours that constrain the architecture

- **Three page layouts** — continuous scroll, single page, two-up — each with its own scroll/paging semantics.
- **Independent page theme vs. app theme.** Page theme (light/sepia/dark) recolours the *document*; app theme recolours the *chrome*. These must never be conflated. Sepia/dark page rendering maps to PDFium's `FPDF_COLORSCHEME` or a post-render GPU shader (the shader is better — see §9).
- **Layer visibility toggles** — highlights, comments, drawings, form fields can each be hidden independently. This demands a real layer model, not baked pixels.
- **Per-document undo/redo stacks** with dirty flags surfaced in tab dots and the status pill.
- **Multi-document tabs** with animated open/close and a document switcher menu.
- **Live find** with match count (`3 of 17`), prev/next, and a distinct "current match" highlight colour.
- **Thumbnail drag-to-reorder** in Organize's grid view.
- **Focus/full-screen mode** that slides the title bar and command bar out (`translateY(-130%)`).
- **Window minimise/maximise/restore** animations driven by the app itself, including a custom "restore" pill.
- **`prefers-reduced-motion`** collapses every duration to 0.01 s. Honour the OS setting on all six platforms.

---

<a name="4-system-architecture"></a>
## 4. System architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│  PLATFORM SHELL                        per-target, thin               │
│  Win32 · AppKit · Wayland/X11 · UIKit · Android NDK · Emscripten      │
│  window chrome · menus · file dialogs · DnD · printing · lifecycle    │
├───────────────────────────────────────────────────────────────────────┤
│  VIEW LAYER                            toolkit-specific, SWAPPABLE    │
│  ├─ folio-ui-kit/      26 QML primitives, token-driven                │
│  ├─ folio-screens/     Welcome, Window, panels, popovers              │
│  └─ FolioPageView      QQuickItem + QSGNode  ← the GPU viewport       │
├───────────────────────────────────────────────────────────────────────┤
│  BINDING LAYER                         the ONLY place Qt meets core   │
│  Q_OBJECT adapters · property notifications · command dispatch        │
╞═══════════════════════════════════════════════════════════════════════╡
│  APPLICATION CORE                      portable C++20, zero toolkit   │
│  SessionManager · DocumentSession · ModeController · Selection        │
│  EditJournal (undo/redo) · SearchService · OutlineModel · NotesModel  │
│  LayerVisibility · RecentFiles · Preferences                          │
├───────────────────────────────────────────────────────────────────────┤
│  RENDER SERVICE                        portable C++20                 │
│  TileScheduler · RasterCache (LRU) · LodPolicy · PrefetchPredictor    │
│  OverlayScene (vector annotations, pre-commit)                        │
├───────────────────────────────────────────────────────────────────────┤
│  ENGINE FAÇADE   IPdfRasterizer · IPdfDocumentStore · IPdfTextIndex   │
│  ├─ PdfiumRasterizer      render · text · forms · links · outline     │
│  └─ PodofoDocumentStore   object model · pages · annots · save        │
├───────────────────────────────────────────────────────────────────────┤
│  PLATFORM SERVICES   fs · fonts · clipboard · crypto · prefs · trace  │
└───────────────────────────────────────────────────────────────────────┘
```

### 4.1 The boundary rule

> **No Qt type crosses into `folio-core`.** No `QString`, no `QObject`, no signals. The core speaks `std::string`, `std::span`, `std::function`, and a small observable primitive.

Enforce it mechanically — a CI step that greps `folio-core/` for `#include <Q` and fails the build. This is what keeps the toolkit swappable, keeps the core unit-testable without a display server, and lets you compile the core for WASM independently of the UI.

### 4.2 Module layout

```
folio/
├── cmake/                       toolchains, triplets, platform presets
├── third_party/                 pdfium (prebuilt), podofo, harfbuzz, ...
├── folio-core/                  portable C++20, no toolkit, no I/O syscalls
│   ├── model/                   Document, Page, Annotation, FormField
│   ├── session/                 DocumentSession, EditJournal, Selection
│   ├── render/                  TileScheduler, RasterCache, LodPolicy
│   ├── engine/                  IPdfRasterizer, IPdfDocumentStore + impls
│   ├── text/                    TextIndex, SearchService, normalisation
│   └── util/                    Observable<T>, SlotMap, Arena, Tracing
├── folio-ui/
│   ├── kit/                     26 primitives (QML + .qrc)
│   ├── screens/                 Welcome.qml, Window.qml, panels, popovers
│   ├── viewport/                FolioPageView (QQuickItem + QSGNode)
│   ├── shaders/                 .qsb — circular reveal, page theme, shadow
│   └── bindings/                Q_OBJECT adapters over core
├── folio-shell/
│   ├── windows/ macos/ linux/ ios/ android/ wasm/
├── tools/
│   ├── token-gen/               design-tokens.json → QML singleton + C++ header
│   └── golden-runner/           cross-platform screenshot diff harness
└── tests/
    ├── core/ render/ golden/ corpus/ perf/ fuzz/
```

---

<a name="5-dual-engine"></a>
## 5. The dual-engine problem: PDFium + PoDoFo under one roof

This is the single most under-estimated part of the project, and getting it wrong produces a class of bug that is very hard to recover from: the pixels on screen stop corresponding to the bytes you will save.

Two libraries, two independent parsers, one document. They must never both believe they own it.

### 5.1 The governing principle

> **PoDoFo owns truth. PDFium owns pixels.**
> PDFium never writes. PoDoFo never renders. Authority transfers only through an explicit, versioned snapshot.

### 5.2 The snapshot protocol

```cpp
// An immutable, ref-counted view of the document bytes at one point in time.
// Built as a rope: [original file] + [incremental update 1] + [update 2] ...
// so a save costs the delta only, never a full re-serialisation.
class DocumentSnapshot {
public:
    uint64_t generation() const noexcept;
    size_t   size()       const noexcept;
    size_t   read(size_t offset, std::span<uint8_t> out) const;  // rope-aware

    // Which pages changed between `from` and this snapshot. Drives cache
    // invalidation: unchanged pages keep their cached tiles across a save.
    const PageIdSet& dirtyPagesSince(uint64_t from) const;
};
using SnapshotPtr = std::shared_ptr<const DocumentSnapshot>;
```

PDFium consumes a snapshot through a custom reader, never a file path:

```cpp
// FPDF_FILEACCESS adapter — lets PDFium read the rope with zero copying.
static int ReadSnapshot(void* param, unsigned long pos,
                        unsigned char* buf, unsigned long size) {
    auto* snap = static_cast<const DocumentSnapshot*>(param);
    return snap->read(pos, {buf, size}) == size ? 1 : 0;
}

FPDF_FILEACCESS fa{};
fa.m_FileLen  = snapshot->size();
fa.m_GetBlock = &ReadSnapshot;
fa.m_Param    = const_cast<DocumentSnapshot*>(snapshot.get());
FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&fa, password);
```

### 5.3 The edit cycle

```
 user gesture
      │
      ▼
 ┌─────────────────────┐   optimistic, GPU-only, zero engine work
 │ OverlayScene        │──────────────────────────────────────────► screen
 │ (live vector draft) │   pen strokes, drag handles, in-flight text
 └─────────┬───────────┘
           │ gesture end  ·  or 400 ms debounce
           ▼
 ┌─────────────────────┐
 │ EditJournal.apply() │  typed, invertible command (undo/redo lives here)
 └─────────┬───────────┘
           ▼
 ┌─────────────────────┐
 │ PodofoDocumentStore │  mutate PdfMemDocument → SaveUpdate() to memory
 │  (podofo thread)    │  → new DocumentSnapshot, generation N+1
 └─────────┬───────────┘
           ▼
 ┌─────────────────────┐  opens gen N+1 in the background while gen N
 │ PdfiumRasterizer    │  keeps serving tiles. Atomic swap when warm;
 │  (pdfium thread)    │  gen N retired once its in-flight tiles drain.
 └─────────┬───────────┘
           ▼
   invalidate ONLY dirtyPagesSince(N) → re-raster → overlay draft retires
```

**Why this shape:** the user never waits on a re-parse. The overlay draft is already on screen at gesture time; the engine round-trip happens behind it and the swap is invisible because the committed raster is pixel-identical to the draft it replaces. This is also precisely what the prototype does — its `.ink` SVG, `.pin`, `.stamp` and `.ff` elements are DOM overlays on top of the page, not part of it.

### 5.4 Authority handoff for form filling

PDFium's form-fill environment (`FPDFDOC_InitFormFillEnvironment`, `FORM_OnLButtonDown`, `FPDF_FFLDraw`) is genuinely excellent — widget hit-testing, focus, field formatting, calculation order, JS actions. Reimplementing it on top of PoDoFo would be months of work and worse.

So form filling is the one bounded window where PDFium is allowed to mutate:

```
  ┌──────────────┐  enter Forms mode   ┌───────────────────┐
  │ PODOFO_OWNS  │────────────────────►│ PDFIUM_FORM_OWNS  │
  │              │◄────────────────────│                   │
  └──────────────┘  commit / leave     └───────────────────┘
                         │
          FPDF_SaveAsCopy(incremental) → buffer
          PoDoFo reloads from buffer, resumes authority
```

**Invariant:** while in `PDFIUM_FORM_OWNS`, every structural edit (page insert/delete/rotate, annotation add) is **rejected or queued**, never applied. Encode this in the type system — `PodofoDocumentStore::beginEdit()` returns `std::expected<EditGuard, EditRejected>` and simply cannot produce a guard in the wrong state. Do not rely on developer discipline; this is the bug that will cost you a week of debugging otherwise.

### 5.5 Annotation appearance streams

PoDoFo writes annotation dictionaries cleanly but does not synthesise appearance streams for every subtype, and a PDF annotation without an `/AP` renders differently in every other viewer.

Rule: **we drew it, so we know its geometry — we emit the `/AP` ourselves.** The `OverlayScene` already holds exact vector geometry for every highlight quad, ink path, stamp and shape. Serialising that to a Form XObject content stream is ~200 lines and gives byte-level control over how Acrobat, Preview and Chrome render your annotations. Never leave `/AP` generation to chance.

---

<a name="6-responsibility-split"></a>
## 6. Responsibility split: who does what

| Capability | Engine | Notes |
|---|---|---|
| Page rasterisation | **PDFium** | `FPDF_RenderPageBitmapWithMatrix` — matrix form, so tiles come free |
| Progressive / interruptible render | **PDFium** | `FPDF_RenderPageBitmap_Start` + `IFSDK_PAUSE` + `_Continue` — the basis of the cooperative scheduler (§7) |
| Text extraction, char boxes | **PDFium** | `FPDFText_LoadPage`, `FPDFText_GetCharBox` — drives selection & search |
| Search | **PDFium → own index** | Use PDFium for extraction; build our own normalised index for Unicode folding, ligatures, hyphen-joins |
| Links / annotations (read) | **PDFium** | `FPDFLink_*`, `FPDFAnnot_*` |
| Bookmarks / outline | **PDFium** | `FPDFBookmark_GetFirstChild` / `_GetNextSibling` |
| Form field interaction | **PDFium** | Full form-fill environment, `FPDF_FFLDraw` |
| Page labels, page size, rotation (read) | **PDFium** | |
| Thumbnails | **PDFium** | Low-priority lane on the same thread |
| Printing raster | **PDFium** | Or pass-through for printers that speak PDF |
| — | — | — |
| Object-graph surgery | **PoDoFo** | `PdfMemDocument`, direct `PdfObject` access |
| Page insert / delete / move / rotate | **PoDoFo** | Preserves everything we don't understand — critical for fidelity |
| Merge / split / extract | **PoDoFo** | |
| Annotation write + `/AP` | **PoDoFo** | Dictionary via PoDoFo, appearance stream synthesised by us |
| Incremental save | **PoDoFo** | `SaveUpdate()` — the backbone of §5.2 |
| Metadata, XMP | **PoDoFo** | |
| Encryption / permissions | **PoDoFo** | |
| Digital signatures | **PoDoFo** | PAdES-B, deferred signing. **Needs OpenSSL — a problem for WASM (§13)** |
| Attachments, embedded files | **PoDoFo** | |
| Optional content groups (layers) | **PoDoFo** | Backs the Layers panel's real-PDF case |
| Watermark / header-footer / Bates | **PoDoFo** | Content-stream injection |
| Compression / optimisation | **PoDoFo** | Object streams, image recompression |
| Font embedding & subsetting | **PoDoFo** | Required for Edit mode's Add Text |

### 6.1 Why both, and not just one

PDFium can edit (`FPDFPageObj_*`, `FPDF_SaveAsCopy`), and PoDoFo can't render — so why not PDFium alone? Because PDFium's editing API is narrow, lossy on constructs it doesn't model, and has no incremental-update story. For an *editor*, preserving the parts of a document you don't understand is the whole game: a legal contract that loses its signature field or an accessibility tag tree on save is a defect, not a rendering nit. PoDoFo's object-tree model plus `SaveUpdate()` is exactly the right tool, and PDFium is exactly the right rasteriser. Using both is correct — it just has to be governed (§5).

---

<a name="7-threading-and-frame-budget"></a>
## 7. Threading model and the frame budget

PDFium's own public header is unambiguous:

> *"NOTE: None of the PDFium APIs are thread-safe. They expect to be called from a single thread."*

Treat that as a hard constraint, including the library-global font cache. The design below respects it without sacrificing throughput.

### 7.1 Threads

| Thread | Owns | Must never |
|---|---|---|
| **UI** | input, QML scene, animation driver, hit-testing | touch PDFium/PoDoFo; allocate >64 KB; block |
| **Qt render** | QSG traversal, GPU submission | call into core |
| **PDFium** (1 per session) | *all* FPDF_* calls, text index, form env | block on PoDoFo |
| **PoDoFo** (1 per session) | `PdfMemDocument`, snapshots, save | call FPDF_* |
| **IO pool** (2–4) | file reads, prefetch, recent-file thumbs | parse PDFs |
| **Worker pool** (N−2) | image decode, OCR, compression, export | call either engine in-process |
| **Out-of-process workers** (0–4) | sandboxed batch raster, untrusted parse | share mutable state |

Everything crosses thread boundaries as **immutable messages over lock-free SPSC queues**. No shared mutable state, no mutex in the frame path.

### 7.2 The cooperative scheduler — the key trick

A single PDFium thread sounds like a bottleneck. It isn't, because PDFium's progressive API lets you **preempt** a long render:

```cpp
struct Pause : FPDF_FILEACCESS_PAUSE_BASE {  // IFSDK_PAUSE
    TileScheduler* sched;
};
static FPDF_BOOL NeedToPause(IFSDK_PAUSE* p) {
    // Yield as soon as a higher-priority tile is queued, or we've spent
    // our slice. The partially-rendered bitmap is preserved for _Continue.
    auto* self = static_cast<Pause*>(p);
    return self->sched->hasHigherPriorityWork() || self->sched->sliceExpired();
}
```

With a ~4 ms slice, a 2-second full-page render of a map-heavy page becomes ~500 resumable fragments. A visible tile that arrives mid-render jumps the queue and lands in the next slice. Result: **the thumbnail panel and the main viewport never starve each other**, from one thread, with no locking.

Priority lanes, highest first:
1. Visible tiles at current zoom
2. Visible-page text index (selection must work immediately)
3. Prefetch tiles (pages N±2, velocity-weighted)
4. Visible thumbnails
5. Off-screen thumbnails
6. Background full-document text index

### 7.3 Frame budget

| | 60 Hz | 120 Hz |
|---|---|---|
| Frame | 16.67 ms | 8.33 ms |
| UI thread input + animation + sync | ≤ 4.0 ms | ≤ 2.5 ms |
| QSG build/batch | ≤ 2.0 ms | ≤ 1.0 ms |
| GPU submit | ≤ 3.0 ms | ≤ 1.5 ms |
| Texture upload (budgeted per frame) | ≤ 2.0 ms | ≤ 1.0 ms |
| Headroom | ≥ 5.6 ms | ≥ 2.3 ms |

Texture upload is *budgeted*, not best-effort: the uploader takes a wall-clock deadline and defers the remainder to the next frame. One 4K tile upload must never cause a dropped frame.

---

<a name="8-render-pipeline"></a>
## 8. The render pipeline: tiles, LOD, caches, GPU upload

### 8.1 Tiling

- Page → grid of **512×512 device-pixel** tiles, snapped to the grid in page space.
- `FPDF_RenderPageBitmapWithMatrix` with a translate+scale matrix and a clip rect renders one tile directly — no render-then-crop.
- **Render opaque pages to `FPDFBitmap_BGRx`**, not BGRA: it matches the GPU's `BGRA8`/`RGBA8` layout for a straight `memcpy` upload and sidesteps premultiplication questions entirely. Use BGRA only for genuinely transparent-group pages, and verify premultiplication against your blend state when you do.

#### 8.1.1 Tiles must be rendered with a bleed margin — **measured, 2026-10-08**

> This subsection was written after the above was tested against real PDFium.
> The claim "renders one tile directly — no render-then-crop" is **wrong for
> text**, and it is wrong in a way that is visible.

Rendering a 612×792 page as 256 px tiles and stitching them does **not**
reproduce the single full-page render:

```
318 differing subpixels, worst channel delta 135/255,
all inside x[256..264] — a 9 px band down one tile boundary
```

A glyph whose **origin** lies outside the tile rasterises differently from the
same glyph drawn with its origin inside the bitmap. PDFium hands FreeType a
different origin; hinting and edge antialiasing diverge. This is not a clip
bug and cannot be fixed by adjusting the clip rect.

**Mitigation:** `TileGrid::bledTile(tx, ty, bleed)` renders an enlarged rect and
the caller copies out the interior. Measured requirement:

| zoom | bleed needed | `recommendedBleed()` |
|---|---|---|
| ×0.5 | 4 px | 12 px |
| ×1 | 8 px | 24 px |
| ×2 | 16 px | 48 px |
| ×4 | residual ±1/255 only | 64 px |

**Revised acceptance criterion (replaces the "byte-identical" wording in §16):**
byte-identical at scale ≤ 2.0; worst channel delta ≤ 1 above it. The residue
above ×2 is PDFium's coverage accumulation rounding against a different bitmap
width — below any display threshold, not worth engineering time.

**Budget impact:** a 512 px tile with 24 px bleed rasterises 560×560, **+19.6%
pixels**. Hold `tileSize ≥ 10 × bleed` — i.e. raise the tile size with zoom —
and the overhead stays near 20%. The test asserts it never exceeds 60%.

**Corollary that bit us:** tile grid coordinates must be carried explicitly on
the request, not derived as `deviceRect.x / tileSize`. With bleed the origin is
`tx·512 − 24`, integer division collapses tx=0 and tx=1 onto 0, and the
scheduler dedupes two different tiles into one. Three quarters of the page
silently fails to render. No unit test finds this; only running the layers
together does.

### 8.2 Level of detail

Three resident levels per page:

| Level | Size | Purpose | Lifetime |
|---|---|---|---|
| **Proxy** | long edge 256 px | instant paint during fling/pinch; thumbnail source | whole session |
| **Screen** | current zoom | the actual view | LRU |
| **Detail** | 2× screen, visible region only | sharp during zoom-in before re-raster settles | transient |

The proxy is why fast scrolling never shows a blank page. It costs ~256 KB per page; even a 1,000-page document fits in 256 MB, and you cap it anyway.

### 8.3 Zoom and scroll

**Never re-raster during an active gesture.** On pinch/wheel/trackpad:

1. Scale existing textures on the GPU (bilinear, free).
2. Accumulate scale into a transform on the `QSGTransformNode`.
3. On gesture end **or** 120 ms of scale stability, enqueue re-raster at the settled scale.
4. Cross-fade new tiles in over 120 ms as they land, so sharpening reads as intentional.

Scroll prefetch is velocity-predicted: `predictedTop = scrollTop + velocity × 0.35s`, prefetch the tiles covering that band plus one page of margin. Cancel prefetches that leave the band — the cooperative scheduler makes cancellation nearly free.

### 8.4 Cache

```cpp
struct TileKey {               // 16 bytes, hashes to a single cache line
    uint32_t pageId;
    uint16_t tileX, tileY;
    uint16_t scaleQ;           // quantised zoom — snap to 1/8 steps so
    uint8_t  rotation;         // small zoom changes reuse tiles
    uint8_t  colorScheme;      // light / sepia / dark
};
```

LRU with a **byte budget**, evicting by `score = distanceFromViewport × ageSeconds / bytes`.

| Platform | Raster budget | GPU texture budget |
|---|---|---|
| Desktop | 256 MB | 384 MB |
| iOS / Android | 96 MB | 128 MB |
| WASM | 64 MB | 96 MB |

Snapping zoom to 1/8 steps for the cache key is a large, cheap win: the pinch gesture generates hundreds of distinct scales, but only ~8 cache generations.

### 8.5 Upload

- Pool `QRhiTexture` objects by size class; never allocate in the frame path.
- Partial updates (`QRhiTextureSubresourceUploadDescription`) — upload the tile, not the atlas.
- Atlas small tiles (thumbnails, proxies) into 2048×2048 pages to keep draw calls low.
- Ring of persistently-mapped staging buffers; the PDFium thread writes straight into mapped memory, so the only copy is DMA.

---

<a name="9-layer-model"></a>
## 9. Layer model: raster · overlay · chrome

The prototype's Layers panel (Highlights / Comments / Drawings / Form fields, independently hideable) is only cheap if layers are real at render time.

```
┌──────────────────────────────────────────┐
│ CHROME          QML scene graph          │  panels, pill, popovers, toasts
├──────────────────────────────────────────┤
│ OVERLAY         GPU vector, per page     │  ← live drafts + hideable layers
│  · selection quads                       │     highlights · comments
│  · find highlights  (#F5B544 / #FF7A45)  │     drawings · form fields
│  · in-flight ink stroke                  │     each independently visible
│  · drag handles, crop frame              │
├──────────────────────────────────────────┤
│ PAGE THEME      fragment shader          │  ← light / sepia / dark
├──────────────────────────────────────────┤
│ RASTER          PDFium tile textures     │  committed document pixels
└──────────────────────────────────────────┘
```

### 9.1 Page theme as a shader, not a re-render

The prototype recolours pages via CSS custom properties (`--pg`, `--pi`). Natively you have two options:

| Approach | Cost | Verdict |
|---|---|---|
| PDFium `FPDF_COLORSCHEME` / `FPDF_RENDER_FORCEHALFTONE` | full re-raster of every visible tile per theme change | ✗ |
| Fragment shader over the existing texture | one uniform change, zero re-raster, animatable | ✓ |

```glsl
// Sepia / dark page theme. Luminance-preserving, runs on the existing tile.
vec3 applyPageTheme(vec3 src, int mode, float mix_) {
    vec3 outc = src;
    if (mode == 1) {                                  // sepia #F1E4C8 / #4A3A22
        float l = dot(src, vec3(0.2126, 0.7152, 0.0722));
        outc = mix(vec3(0.290,0.227,0.133), vec3(0.945,0.894,0.784), l);
    } else if (mode == 2) {                           // dark  #22252E / #E1E3EA
        float l = dot(src, vec3(0.2126, 0.7152, 0.0722));
        outc = mix(vec3(0.133,0.145,0.180), vec3(0.882,0.890,0.918), l);
    }
    return mix(src, outc, mix_);                      // mix_ animates the swap
}
```

Because `mix_` is animatable, the page-theme change becomes a smooth 400 ms cross-fade matching the prototype's `transition: background .4s` — something a re-render approach cannot do at all.

### 9.2 Why overlays stay vector until commit

- Infinite sharpness at any zoom with zero re-raster.
- Hit-testing is geometric, so hover/select is exact.
- Layer visibility toggles are a per-layer boolean, not a cache invalidation.
- The pen stroke under the user's finger has **zero engine latency** — it is a GPU polyline appended per input event, exactly like the prototype's live `<path>` growth.

---

<a name="10-animation-system"></a>
## 10. Animation system — mapping every motion in the demo

The prototype's shared easing is `cubic-bezier(.42, 0, .58, 1)` — a symmetric ease-in-out. In QML: `easing.type: Easing.Bezier; easing.bezierCurve: [0.42, 0, 0.58, 1, 1, 1]`. Define it once as `Tokens.ez`.

### 10.1 Complete motion inventory

| # | Element | CSS in prototype | Native implementation |
|---|---|---|---|
| 1 | Button hover/press | `background .2s`, `transform .15s`, `:active scale(.93)` | `Behavior on scale` + `ScaleAnimator` (render thread) |
| 2 | Tab open | `@keyframes tin` width 0→178, opacity, padding, `.35s` | `NumberAnimation` on `implicitWidth` + `OpacityAnimator` |
| 3 | Tab close | `.cl` width→0, opacity→0, `.3s`, then splice | `SequentialAnimation` → `ScriptAction` removes model row |
| 4 | Screen swap | `.scr.off` opacity + `scale(.97)`, `.5s` | `ParallelAnimation`, `OpacityAnimator` + `ScaleAnimator` |
| 5 | Minimise | `.mn` `translateY(80px) scale(.5)`, opacity 0 | `YAnimator` + `ScaleAnimator` + `OpacityAnimator` |
| 6 | Maximise/restore | `#app.rs` `scale(.93)`, inset 10px, radius 16 | animate `scale`, `anchors.margins`, `radius` |
| 7 | Reading progress bar | `width .5s` | `Behavior on width` |
| 8 | Focus mode | `#title/#cmd translateY(-130%)` `.5s` + opacity `.4s` | `YAnimator` + `OpacityAnimator`; `#work.top` 112→12 |
| 9 | Tool strip swap | `#tools.out` opacity + `translateY(-6px)` `.16s`, swap at 160 ms | `SequentialAnimation`: fade out → `ScriptAction` → fade in |
| 10 | **Active tool label grow** | `.tb .lab max-width 0→130px .35s` + opacity `.25s` | `NumberAnimation` on `Layout.preferredWidth`, `clip: true` |
| 11 | Panel collapse | `width/height/margin .34s`, 268⇄48 px | `Behavior on implicitWidth` + `Behavior on implicitHeight` |
| 12 | **Panel circular reveal** | `clip-path: circle(--r at --ox --oy)` `.34s` | **`ShaderEffect` SDF mask**; origin = clicked button centre |
| 13 | Rail fade | opacity `.12s` out / `.2s` in with `.16s` delay | `OpacityAnimator` + `PauseAnimation` |
| 14 | Left pane switch | opacity `.3s` + `translateY(8px)` | `OpacityAnimator` + `YAnimator` |
| 15 | Right panel switch | opacity `.3s` + `translateX(12px)` | `OpacityAnimator` + `XAnimator` |
| 16 | Toggle switch | track colour `.3s`, knob `translateX(14px)` `.3s` | `Behavior on color` + `XAnimator` |
| 17 | Colour swatch | `scale(1.12)` hover `.2s`, ring `.2s` | `ScaleAnimator` + `Behavior on border.width` |
| 18 | Thumbnail hover | `translateY(-2px)` `.2s` | `YAnimator` |
| 19 | Thumbnail delete | `.gone scale(.6)` + opacity 0, `.3s`, then splice | `SequentialAnimation` → model removal |
| 20 | **Page rotate** | `transform: rotate(--rot) scale(--rs)`, `.5s`; `--rs = .72` when landscape | `RotationAnimator` + `ScaleAnimator`; shrink keeps page in frame |
| 21 | Page width (zoom) | `width .4s` | `Behavior on width` — only for discrete zoom steps, never gestures |
| 22 | Page entrance | `@keyframes pgin` opacity + `translateY(14px)`, `.5s`, staggered | `Repeater` + `PauseAnimation { duration: index*40 }` |
| 23 | Chart bar grow | `@keyframes grow scaleY(0→1)` `.8s` | demo-content only; N/A for real PDFs |
| 24 | Annotation placement | `@keyframes pi` opacity + `scale(.5→1)` `.4s` | `ScaleAnimator` + `OpacityAnimator` on overlay item |
| 25 | **Popover reveal** | `clip-path: circle()` `.46s` from trigger centre | same `ShaderEffect` mask as #12 |
| 26 | Mode chevron | `rotate(180deg)` `.3s` | `RotationAnimator` |
| 27 | Theme icon spin | `@keyframes spn rotate(-120°) scale(.4)→1` `.6s` | `RotationAnimator` + `ScaleAnimator` |
| 28 | **Theme change** | View Transition API, circular clip, **760 ms** | §10.3 — snapshot + animated circular mask |
| 29 | Tooltip | opacity `.18s` + `translateY(4px) scale(.96)`; **380 ms hover delay** | `Timer(380)` → `ParallelAnimation` |
| 30 | Toast | opacity `.25s` + `translateY(10px)`; auto-hide **1700 ms** | `SequentialAnimation` with `PauseAnimation` |
| 31 | Recent card hover | `translateY(-4px)` `.3s` + shadow grow | `YAnimator` + `Behavior` on shadow params |
| 32 | Recent card entrance | staggered `80 + i×50 ms` | `PauseAnimation { duration: 80 + index*50 }` |
| 33 | Hero progress fill | `@keyframes fillbar width 0→pct` `.9s` | `NumberAnimation` |
| 34 | Drop zone | `scale(1.01)` + colour `.3s` | `ScaleAnimator` + `Behavior on color` |
| 35 | Remove button | opacity 0→1 + `scale(.7→1)` `.2s` | `ScaleAnimator` + `OpacityAnimator` |
| 36 | Organize crossfade | grid opacity 0→1 `scale(.97→1)`; stage opacity→0 `scale(1.03)` | `ParallelAnimation` across both items |
| 37 | Restore pill | `translate(-50%, 40px)→0` `.45s` | `YAnimator` + `OpacityAnimator` |
| 38 | Reduced motion | all durations → `0.01s` | global `Tokens.motionScale` multiplier, 0 when OS asks |

### 10.2 Use `Animator`, not `NumberAnimation`, for transforms

This is the single most important performance rule in the UI layer. Qt Quick's `Animator` types (`XAnimator`, `YAnimator`, `ScaleAnimator`, `OpacityAnimator`, `RotationAnimator`) are evaluated **on the render thread**. They keep running smoothly even when the UI thread is busy — e.g. while a 400-page document's outline is being built.

```qml
// ✗ UI thread — stutters when the app is loading something
Behavior on y { NumberAnimation { duration: 340; easing: Tokens.ez } }

// ✓ render thread — immune to UI-thread stalls
YAnimator on y { duration: 340; easing.type: Easing.Bezier
                 easing.bezierCurve: Tokens.ezCurve }
```

Reserve `NumberAnimation` for properties `Animator` can't drive (width, colour, custom shader uniforms).

### 10.3 The two signature effects

**Circular reveal** (panels #12, popovers #25). A reusable `ShaderEffect` mask whose origin is the clicked control's centre, exactly as the prototype computes `--ox`/`--oy` from `getBoundingClientRect()`:

```glsl
// circular_reveal.frag  →  compiled to .qsb
layout(location = 0) in  vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D src;
layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    vec2  center;     // in item coords, from the trigger's centre
    vec2  itemSize;
    float radius;     // animated 0 → hypot(furthest corner)
    float feather;    // 1.5 px — kills the aliased edge
};
void main() {
    float d = distance(qt_TexCoord0 * itemSize, center);
    float a = 1.0 - smoothstep(radius - feather, radius + feather, d);
    fragColor = texture(src, qt_TexCoord0) * qt_Opacity * a;
}
```

Drive `radius` with a `NumberAnimation` (340 ms panels / 460 ms popovers). The radius target is `hypot(max(ox, w-ox), max(oy, h-oy)) + 8` — identical to the prototype's formula.

**Theme transition** (#28). The web version uses the View Transition API. The native equivalent:

1. `ShaderEffectSource { live: false }` captures the current frame to a texture.
2. Swap the token singleton to the new theme — everything below repaints instantly.
3. Draw the captured snapshot on top, masked by the same circular shader.
4. Animate `radius` over **760 ms**. Dark→light expands; light→dark contracts (matching `vtd`/`vtl`).
5. Drop the snapshot.

One full-screen texture, one shader, 760 ms. Works identically on all six platforms, including WASM.

---

<a name="11-design-tokens"></a>
## 11. Design tokens and the component kit

### 11.1 Single source of truth

`design-tokens.json` is the only place a colour or duration is written. `tools/token-gen` generates, at build time:

- `Tokens.qml` (QML singleton)
- `folio_tokens.h` (`constexpr` C++)
- `tokens.css` (keeps the HTML prototype in sync — it stays useful as a living reference)

CI fails on any hard-coded hex literal or duration in `folio-ui/`. This is how "same design everywhere" stops being a hope.

Values transcribed from the prototype:

| Token | Light | Dark |
|---|---|---|
| `bg` background | `#EDEAE3` | `#0F1016` |
| `cd` card | `#FFFFFF` | `#1B1D28` |
| `tx` text | `#2A2F45` | `#E7E9F2` |
| `mu` muted | `#5E6382` | `#A3A9C2` |
| `bd` border | `#DAD9EA` | `#3A3D52` |
| `dv` divider | `#ECEBF3` | `#2C2F40` |
| `ac` accent | `#4338CA` | `#5B5BE0` |
| `as` accent-soft | `#ECEBFA` | `#2A2C58` |
| `at` accent-text | `#3730A3` | `#C3C5FF` |
| `sg` surface-sunken | `#F3F2FA` | `#252838` |
| `hv` hover | `#EEEDF7` | `#272B3D` |
| `dg` danger | `#B42318` | `#FF8C85` |
| `sh` shadow | `rgba(40,36,90,.16)` | `rgba(0,0,0,.55)` |
| `rg` ring | `rgba(40,36,90,.06)` | `rgba(255,255,255,.07)` |
| `ps` page-shadow | `rgba(40,36,90,.20)` | `rgba(0,0,0,.65)` |
| `pg` page | `#FEFEFE` | — (page theme is independent) |
| `pi` page-ink | `#2A2F46` | — |

Page themes: sepia `#F1E4C8` / `#4A3A22`; dark `#22252E` / `#E1E3EA`.
Geometry: radii 6/8/9/10/11/12/14/16/18/21 px; control height 34 px; command bar 48 px; title bar 44 px; left panel 268 px; right panel 272 px; collapsed rail 48 px; pill height 42 px.
Typography: **DM Sans** 400/500/600/700 — ship the font (SIL OFL 1.1) on every platform so text metrics are byte-identical. Never fall back to a system UI font; that alone would break pixel parity.

### 11.2 The 26 primitives

`Button` · `IconButton` · `ToggleButton` · `SegmentedControl` · `Card` · `Divider` · `SectionHeader` · `TextField` · `NumberField` · `Select` · `Slider` · `Switch` · `ColorSwatch` · `Tab` · `TabStrip` · `Popover` · `Menu` · `MenuItem` · `Tooltip` · `Toast` · `ProgressBar` · `Panel` · `CollapsibleRail` · `ThumbnailCell` · `PageCell` · `DropZone`

Each gets: a token-only style, every state from the prototype (rest / hover / active / on / disabled / focus-visible), keyboard semantics, an accessibility role, and a golden-image test per theme. 26 components × 2 themes = 52 reference images that must match on all six platforms.

---

<a name="12-platform-shells"></a>
## 12. Platform shells and adaptive layout

### 12.1 Window chrome

The prototype draws its own title bar with minimise/maximise/close. Native reality:

| Platform | Approach | Gotchas |
|---|---|---|
| **Windows** | `WS_THICKFRAME` + custom `WM_NCCALCSIZE`, `WM_NCHITTEST` | Must return `HTMAXBUTTON` on maximise-button hover or **Win11 Snap Layouts break**. Preserve the DWM shadow, handle per-monitor DPI v2. |
| **macOS** | `titlebarAppearsTransparent` + `fullSizeContentView` | **Decision required** — see below. Respect `NSWindow` full-screen and Stage Manager. |
| **Linux** | Client-side decorations | Wayland `xdg-toplevel` resize edges; X11 needs `_GTK_FRAME_EXTENTS`; detect compositors that force SSD and degrade gracefully. |
| **iOS / Android** | No chrome | Title bar → navigation bar; honour `safe-area-inset-*` (already referenced in the prototype's CSS). |
| **WASM** | No chrome | Full-viewport canvas; hide the window controls entirely. |

> **Open decision — macOS traffic lights.** "Identical design everywhere" and macOS convention conflict here. Options: (a) draw Folio's own controls on macOS too — perfectly consistent, mildly alien to Mac users; (b) show native traffic lights positioned to match the prototype's left-gutter rhythm. Recommendation: **(b)**, as the one sanctioned deviation. macOS users notice missing traffic lights immediately, and window management is muscle memory, not branding. Flagging rather than silently choosing.

### 12.2 Adaptive layout

The prototype is a fixed 1440×900 canvas that `transform: scale()`s to fit. That is right for a demo and wrong for a product. Keep every token identical; reflow only the layout:

| Breakpoint | Layout |
|---|---|
| ≥ 1280 px | Three columns: 268 px panel · stage · 272 px panel (the prototype exactly) |
| 1024–1280 | Panels overlay the stage instead of pushing it; auto-collapse to rails |
| 768–1024 (tablet) | One panel at a time as a drawer; pill stays floating |
| < 768 (phone) | Panels become full-height sheets; tool strip becomes a bottom sheet; pill becomes a bottom bar; title bar becomes a nav bar |

Touch targets grow to 44 pt on mobile while visual sizing stays identical — expand hit areas, not pixels.

### 12.3 Input parity

| Input | Desktop | Touch |
|---|---|---|
| Zoom | Ctrl+wheel, trackpad pinch, ±, Fit Width | two-finger pinch, double-tap-to-fit |
| Pan | wheel, two-finger scroll, space-drag | one-finger drag with momentum |
| Select text | click-drag, double-click word, triple-click para | long-press + handles |
| Annotate | tool + click/drag | tool + touch; palm rejection; **Apple Pencil / S Pen pressure → stroke width** |
| Context | right-click | long-press |

Pointer-event abstraction lives in core; only the shell knows about `NSEvent` vs `MotionEvent`.

---

<a name="13-webassembly"></a>
## 13. WebAssembly: the hard target

This target will take longer than you expect. Plan for it explicitly.

### 13.1 Threading

PDFium and PoDoFo both want threads. In WASM, pthreads require `SharedArrayBuffer`, which requires cross-origin isolation:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Every embedded third-party resource must then send `Cross-Origin-Resource-Policy: cross-origin`. This is a **deployment constraint, not a code constraint** — it must be agreed with whoever owns hosting before Phase 8, because it can break analytics scripts, embedded fonts and ad tags.

Fallback when isolation is impossible: single-threaded with Asyncify, using the same `IFSDK_PAUSE` cooperative scheduler from §7.2 to yield to the browser event loop every ~4 ms. The architecture already supports this — the scheduler was designed for exactly this shape. Expect roughly 40% of the throughput.

### 13.2 Binary size

| Component | Approx. gzipped |
|---|---|
| PDFium, XFA+V8 off | 2.5–3.5 MB |
| PDFium, XFA+V8 on | 9–12 MB |
| PoDoFo + freetype + zlib + libpng + libjpeg | 1.5–2.5 MB |
| PoDoFo + OpenSSL (signing) | +2.5 MB |
| Qt Quick runtime | 8–15 MB |
| Folio app code | 1–2 MB |

Build PDFium with `pdf_enable_xfa=false pdf_enable_v8=false`. **Trade-off:** you lose XFA forms and JavaScript form actions on web only. For a reader/editor this is the right call; make it a documented, deliberate difference. Ship `-Oz`, split the engine into a lazily-fetched module so first paint doesn't wait for it, and stream the wasm with `instantiateStreaming`.

### 13.3 Dependency reality check

| Dependency | Emscripten status |
|---|---|
| zlib, libpng, libjpeg-turbo, freetype | emscripten-ports — easy |
| libxml2 | buildable, some configure work |
| OpenSSL | heavy; **consider dropping signing on web** |
| fontconfig | not applicable; stub it out, ship fonts in MEMFS |

Recommendation: a `FOLIO_WASM_MINIMAL` build that drops signing and encryption-at-rest. Document which features are desktop/mobile only — web is the reader + annotator + organiser, not the full editor.

### 13.4 Storage and files

File System Access API where available (Chromium), `<input type=file>` + download fallback elsewhere. Persist recents and preferences in IndexedDB via Emscripten's IDBFS, with an explicit sync point — IDBFS does not auto-flush.

---

<a name="14-performance"></a>
## 14. Performance engineering playbook

### 14.1 Fixed budgets

| Metric | Target | Fail gate |
|---|---|---|
| Cold start → Welcome interactive | < 400 ms desktop / < 900 ms mobile | 2× |
| Open 50-page PDF → first page painted | < 250 ms | 500 ms |
| Open 1000-page PDF → first page painted | < 600 ms | 1.2 s |
| Scroll frame time p99 | < 16.6 ms | > 20 ms |
| Pinch-zoom frame time p99 | < 16.6 ms | > 20 ms |
| Mode switch (full animation) | < 16.6 ms/frame over 500 ms | any drop |
| Panel collapse reveal | 0 dropped frames | 1 |
| Idle CPU, document open | < 0.5% | 2% |
| RSS, 100-page doc open | < 400 MB desktop / < 180 MB mobile | 1.5× |

p99, not mean. Mean frame time hides exactly the stutter users complain about.

### 14.2 Specific wins, ordered by value

1. **Icon SDF atlas.** The prototype has ~60 stroked SVG icons. Rasterise them once into a single signed-distance-field atlas at startup (~3 ms) → every icon in the UI draws from one texture, one material, one batch, and scales crisply to any DPI. Without this you get 60 separate materials and QSG batching collapses.
2. **`Animator` for all transforms** (§10.2).
3. **`layer.enabled` discipline.** Each layer allocates an FBO and breaks batching. Use it *only* for the circular-reveal masks and the theme-transition snapshot. Audit in CI via `QSG_RENDERER_DEBUG=render`.
4. **Item reuse in thumbnail lists.** `TableView`/`ListView` with `reuseItems: true` and a tuned `cacheBuffer`. A 1,000-page document must instantiate ~30 delegates, not 1,000.
5. **Zoom quantisation** (§8.4) — turns hundreds of cache generations into ~8.
6. **No allocation in the frame path.** Arena allocators for per-frame scratch; pool tile buffers and textures by size class.
7. **Text index off the critical path.** Build it page-by-page at priority 6 so a huge document is searchable progressively; the find UI shows a growing count, which reads as responsiveness rather than lag.
8. **Avoid `QQuickPaintedItem` entirely.** It goes through a software raster path and an extra texture upload. Everything custom is `QSGNode`.
9. **Opaque geometry first.** Keep the page quad opaque and let QSG's opaque pass depth-reject the content behind it; don't make the whole viewport alpha-blended for the sake of one rounded corner.

### 14.3 Instrumentation

Ship tracing in release builds behind a flag: Perfetto on Android/Linux/Windows, `os_signpost` on Apple, `console.timeStamp` on web. Trace points on every stage boundary (gesture → schedule → raster → upload → present). A field report of "scrolling is janky" should resolve into a flame graph, not a repro hunt.

---

<a name="15-build-and-ci"></a>
## 15. Build system, dependencies, CI

### 15.1 CMake

CMake ≥ 3.28, C++20, Ninja, presets per target. One `CMakePresets.json` with 12 configurations (6 platforms × Debug/Release) so a developer's local build and CI are the same command.

### 15.2 Dependency acquisition

**Do not attempt to build PDFium inside your CMake tree.** It is a GN/Chromium build with its own toolchain, `depot_tools`, and a multi-GB checkout. Instead:

- Consume prebuilt binaries (e.g. `bblanchon/pdfium-binaries`) for day-one velocity, **or**
- Run a separate, slow pipeline that builds PDFium per platform with pinned GN args and publishes versioned artefacts to an internal package registry.

Recommendation: start with prebuilts, stand up your own pipeline by Phase 3 — you will eventually need custom GN args (`pdf_enable_xfa`, `pdf_enable_v8`, `pdf_use_skia`) and a pinned, auditable revision. Note that PDFium's Skia backend remains the non-default, less-mature path; default AGG is the safe choice unless the spike shows a clear win.

PoDoFo, freetype, zlib, libxml2, OpenSSL, libjpeg-turbo, libpng, libtiff via **vcpkg manifest mode** with custom triplets (`arm64-ios`, `wasm32-emscripten`, `arm64-android`).

### 15.3 CI matrix

| Target | Runner | Notes |
|---|---|---|
| Windows x64 / arm64 | windows-latest | MSVC 19.4x |
| macOS x64 / arm64 | macos-14 | universal binary, notarisation |
| Linux x64 / arm64 | ubuntu-22.04 | AppImage + Flatpak |
| iOS arm64 | macos-14 | device + simulator |
| Android arm64/x86_64 | ubuntu + NDK r27 | |
| WASM | ubuntu + emsdk | pinned emsdk version |

Every PR: build all 12 configs, run core unit tests, run golden-image tests on the three desktop platforms, run the perf gate on a dedicated machine (not a shared runner — frame-time gates on noisy hardware produce flaky failures and get disabled, which defeats the purpose).

---

<a name="16-testing"></a>
## 16. Testing — including mechanical enforcement of "same UI everywhere"

### 16.1 Golden-image tests: the core of the pixel-identity requirement

"Same UI design everywhere" is only real if it is *measured*. The harness:

1. Boots the app headless (offscreen RHI) at a fixed 1440×900 and fixed DPR.
2. Drives a scripted sequence through every screen, mode, panel state and theme.
3. Captures PNGs at fixed animation timestamps (freeze the animation clock and step it deterministically — never sleep).
4. Compares against references with a perceptual metric (ΔE2000, tolerance 1.0; fail on >0.1% of pixels exceeding it).
5. **Compares each platform against the others**, not just against its own baseline. Drift between macOS and Windows is the failure mode you actually care about.

~180 golden images (26 components × 2 themes + ~70 screen states × 2 themes). Run on every PR for desktop; nightly for mobile and WASM.

Freezing the animation clock is what makes this tractable: with a deterministic `AnimationDriver`, you can assert the exact frame at t=170 ms of the panel reveal, which catches easing-curve regressions that a settled-state screenshot would miss entirely.

### 16.2 Other layers

| Layer | Approach |
|---|---|
| Core unit tests | GoogleTest; no display, no engine — mock `IPdfRasterizer` |
| Engine integration | Real PDFium/PoDoFo against a pinned corpus |
| **PDF corpus** | PDFium test corpus + pdf.js corpus + veraPDF + your own pathological set. Assert: renders without crash, round-trips through PoDoFo byte-stable, text extraction matches reference |
| **Round-trip fidelity** | Open → save with no edits → the output must be byte-identical (incremental update with an empty delta). Any diff is a silent-corruption bug. |
| Interaction | Scripted QML tests on the real scene graph |
| Perf regression | Frame-time p99 on fixed hardware; gate at 10% regression |
| Fuzzing | libFuzzer on the engine façade; OSS-Fuzz-style corpus minimisation; run continuously, not per-PR |
| Accessibility | Automated role/label audit + manual screen-reader passes (NVDA, VoiceOver, TalkBack) per release |

The round-trip fidelity test deserves emphasis: it is cheap to write, and it catches the worst class of editor bug — the one where a user opens a document, changes nothing, saves, and silently loses a signature or a tagged-PDF structure tree.

---

<a name="17-security"></a>
## 17. Security

PDFs are hostile input. PDFium has a long CVE history precisely because it is the most-attacked PDF parser in the world; PoDoFo gets far less fuzzing attention.

- **Out-of-process parsing** for untrusted documents. The façade already supports remote workers (§7.1) — use it. One worker process per document, crash-isolated, auto-restarted.
- **Sandbox** the worker: seccomp-bpf (Linux), AppContainer + Job object (Windows), App Sandbox + XPC (macOS), platform sandbox (iOS/Android), browser sandbox (WASM).
- **Disable V8** unless form JS is a requirement. If enabled, it must only ever run in the sandboxed worker.
- **No network from the engine.** Strip link-following, remote resource fetch and `/SubmitForm` actions at the façade — mediate every one through an explicit user prompt in the UI layer.
- **Encryption:** support AES-256 (PDF 2.0) via PoDoFo; never log passwords; zeroise key material.
- **Signatures:** PoDoFo's PAdES-B support with deferred signing so the private key can live in a platform keystore / HSM and never enter process memory.
- Pin PDFium revisions and track upstream security releases; a stale PDFium is a liability.

---

<a name="18-risks"></a>
## 18. Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Qt requires a commercial licence for 2 of 6 targets** — WASM is GPLv3-only, iOS is static-link-only with a contested LGPLv3 position | Certain | High | Budget the commercial licence, or keep the core toolkit-agnostic (§4.1) and move the whole UI to Slint. Decide at Phase 0, not later. See §2.1–2.2. |
| 1b | Qt static-only on iOS also forces manual static QML plugin registration | Certain | Low | Handle in the Phase 0 spike; blank-screen-on-device is the classic symptom |
| 2 | Dual-engine divergence — screen ≠ saved bytes | High | Critical | §5 snapshot protocol; round-trip fidelity tests (§16.2); authority state machine enforced by types |
| 3 | PDFium single-thread bottleneck | Medium | High | Cooperative `IFSDK_PAUSE` scheduler (§7.2); out-of-process workers for batch |
| 4 | WASM build larger/slower than acceptable | High | Medium | Strip XFA+V8; lazy engine module; `FOLIO_WASM_MINIMAL`; set expectations early |
| 5 | iOS maturity of the chosen toolkit | Medium | High | Phase 0 spike builds on a real device before committing |
| 6 | Edit mode scope explosion (text reflow, font substitution) | High | High | Constrain v1: edit existing text runs in place, no reflow across lines. Say so in the spec. |
| 7 | Pixel drift between platforms | High | Medium | Ship DM Sans; distance-field text; golden-image cross-platform diffs (§16.1) |
| 8 | 60 fps missed on low-end Android | Medium | Medium | Proxy LOD always resident; reduced-motion tier auto-enabled below a perf threshold |
| 9 | PoDoFo signing needs OpenSSL on WASM | High | Low | Drop signing on web; documented platform difference |
| 10 | PDFium prebuilt binaries diverge from needed GN args | Medium | Medium | Own the build pipeline by Phase 3 |
| 11 | macOS traffic-light conflict | Certain | Low | Explicit product decision (§12.1) |
| 12 | Cross-origin isolation blocked by hosting | Medium | High | Confirm with hosting owners before Phase 8; Asyncify fallback ready |

---

<a name="19-roadmap"></a>
## 19. Roadmap and effort

Team of five: 2 UI, 2 core/engine, 1 platform/build.

| Phase | Weeks | Deliverable | Exit criteria |
|---|---|---|---|
| **0 — De-risk** | 4 | Tile viewport spike in **both** Qt Quick and Slint; WASM build of each; PDFium+PoDoFo linked on all six targets | 120 fps scroll on a 500-page doc, desktop + one mobile device; toolkit decision signed off |
| **1 — Foundations** | 6 | Token pipeline, 26 primitives, golden harness, CI matrix, window shells (Win/mac/Linux) | 52 component goldens matching across three platforms |
| **2 — Read mode** | 8 | Open/render/scroll/zoom, three layouts, page themes, outline, thumbnails, find, tabs, Welcome screen | All read-path perf gates green |
| **3 — Annotate** | 6 | Six tools, overlay layer, `/AP` generation, Notes panel, layer toggles, undo/redo | Annotations round-trip into Acrobat/Preview correctly |
| **4 — Organize** | 4 | Grid view, multi-select, rotate/dup/insert/extract/delete, drag-reorder, merge/split | Round-trip fidelity suite green |
| **5 — Edit** | 10 | Text edit in place, add text, image, link, shape, redact, crop, header/footer, watermark, Bates | Font subsetting correct; redaction genuinely destroys content |
| **6 — Forms** | 6 | Field placement, fill session, authority handoff, flatten | Authority state machine has no escape path |
| **7 — Mobile** | 8 | iOS + Android shells, adaptive layout, touch/stylus input | Mobile goldens + perf gates green |
| **8 — Web** | 6 | WASM build, cross-origin isolation, IDBFS, lazy engine module | Reader + annotate + organise functional on web |
| **9 — Hardening** | 8 | Accessibility, localisation (incl. RTL), fuzzing, security review, installers, auto-update | Screen-reader pass on all platforms; no P1 fuzz findings |

**Total: ~66 weeks ≈ 16 months**, plus buffer → 16–18 months.

Parallelisation: Phases 3–6 overlap substantially (different modes, mostly independent). Phase 7 can start during Phase 5 with a dedicated platform engineer. Phase 9 should run continuously rather than as a trailing block — accessibility and localisation retrofits are far more expensive than doing them inline.

### Earliest credible milestones

- **Month 2:** internal demo — real PDF, smooth scroll, correct chrome, two themes.
- **Month 5:** desktop Read-mode beta, three platforms, pixel-verified.
- **Month 9:** annotate + organise feature-complete on desktop.
- **Month 13:** mobile beta.
- **Month 16–18:** all six platforms, all five modes.

---

<a name="appendix-a-licensing"></a>
## Appendix A — Licensing

You asked me to flag the risks rather than assume a model. Here is the complete picture.

| Dependency | Licence | Proprietary-safe? | Notes |
|---|---|---|---|
| **PDFium** | BSD-3-Clause | ✅ Yes | Cleanest in the stack. Attribution only. |
| **PoDoFo 1.0 (library)** | **LGPL-2.0+ *or* MPL-2.0, your choice** | ✅ **Yes — take MPL-2.0** | MPL-2.0 is file-level copyleft: static linking into proprietary code is fine, including on iOS; you only publish changes to PoDoFo's own files. No object-file deposit. **Requires 1.0+ — 0.10.x is LGPL-only.** |
| PoDoFo **tools** | GPL-2.0+ | ⚠️ Do not ship | Library only. Also consider disabling AFDKO integration. |
| **Qt Quick (desktop)** | LGPL-3.0 / commercial | ⚠️ Conditional | Dynamic linking is clean. Static linking is permitted by the LGPL if you ship relinkable object files, plus the right to reverse-engineer for debugging. |
| **Qt Quick (iOS)** | LGPL-3.0 / commercial | ❌ Effectively no | **Qt refuses to build shared for iOS** — static is forced (§2.1). LGPLv3 additionally requires the user be able to *run* the relinked binary, which drags in signing/provisioning. Contested; Qt's own position is "buy commercial". **Treat as a commercial target.** |
| **Qt Quick (Android)** | LGPL-3.0 / commercial | ✅ Conditional | Android *does* support shared Qt libs, so standard LGPL dynamic linking applies here. |
| **Qt for WebAssembly** | **GPL-3.0 / commercial — no LGPL** | ❌ No | **Confirmed.** A proprietary web build requires a commercial Qt licence. The largest licence risk in the project. |
| Slint | Royalty-free (proprietary desktop/mobile/web) · GPL-3.0 · commercial (embedded) | ✅ Yes | Notably cleaner than Qt for exactly your target set. |
| Skia | BSD-3-Clause | ✅ Yes | If you go the custom-engine route. |
| FreeType | FTL or GPL-2.0 | ✅ Yes | Use FTL; attribution required. |
| HarfBuzz | MIT | ✅ Yes | |
| zlib / libpng / libjpeg-turbo | permissive | ✅ Yes | |
| libxml2 | MIT | ✅ Yes | |
| OpenSSL 3.x | Apache-2.0 | ✅ Yes | |
| **DM Sans** | SIL OFL 1.1 | ✅ Yes | Redistributable with the app; cannot be sold standalone. Ship it — it is load-bearing for pixel parity. |

### The decision this forces

| If Folio is… | Recommended path |
|---|---|
| Open source (GPL-compatible) | Qt everywhere, zero licence cost |
| Proprietary, **no web, no iOS** | Qt under LGPL with dynamic linking on desktop + Android. No licence cost. |
| Proprietary, **iOS but no web** | Commercial Qt licence for the iOS target (static linking, §2.1) |
| **Proprietary, with web and iOS** | Commercial Qt licence covering both — **or** move the whole UI to Slint (royalty-free for proprietary desktop + mobile + web) |
| Internal only, not distributed | Licensing is largely moot; pick on engineering merit alone |

Note the asymmetry: **Android is fine under LGPL** (Qt ships shared libraries there), while **iOS is not** (static only). The two mobile platforms have genuinely different licence positions, which is counter-intuitive and worth stating explicitly to whoever signs off.

**Not a licence decision, but adjacent:** PDFium is BSD-3 and PoDoFo under MPL-2.0 is static-link-safe, so **the two PDF engines impose no constraints on any platform**. Every licensing decision in this project is a Qt decision.

> Not legal advice. Have counsel review before shipping — particularly the LGPL-3.0 static-linking position on iOS, which is genuinely contested.

---

<a name="appendix-b-api"></a>
## Appendix B — Engine API cheat-sheet

### The façade

```cpp
namespace folio::engine {

struct TileRequest {
    PageId    page;
    RectI     tileDevice;     // device-pixel rect within the page
    float     scale;
    int       rotation;       // 0/90/180/270
    ColorScheme scheme;
    Priority  priority;
    uint64_t  snapshotGen;    // stale requests are dropped on arrival
};

struct TileResult {
    TileKey                key;
    std::span<const std::byte> bgrx;  // owned by the pool, not the caller
    size_t                 stride;
    bool                   partial;   // progressive: more is coming
};

class IPdfRasterizer {
public:
    virtual ~IPdfRasterizer() = default;
    virtual void     setSnapshot(SnapshotPtr) = 0;     // atomic swap
    virtual void     submit(TileRequest) = 0;
    virtual void     cancel(PageId, uint64_t olderThanGen) = 0;
    virtual PageInfo pageInfo(PageId) const = 0;
    virtual const TextIndex* textIndex(PageId) = 0;    // nullptr until ready
};

class IPdfDocumentStore {
public:
    virtual ~IPdfDocumentStore() = default;
    virtual std::expected<EditGuard, EditRejected> beginEdit() = 0;
    virtual SnapshotPtr snapshot() const = 0;          // current generation
    virtual std::expected<void, SaveError> saveIncremental(Sink&) = 0;
    virtual std::expected<void, SaveError> saveLinearized(Sink&) = 0;
};

} // namespace folio::engine
```

### PDFium calls, by feature

| Feature | Calls |
|---|---|
| Init / teardown | `FPDF_InitLibraryWithConfig`, `FPDF_DestroyLibrary` (once, on the pdfium thread) |
| Load from snapshot | `FPDF_LoadCustomDocument` + `FPDF_FILEACCESS` |
| Page | `FPDF_LoadPage`, `FPDF_GetPageWidthF/HeightF`, `FPDF_ClosePage` |
| Tile render | `FPDFBitmap_CreateEx` (external buffer!), `FPDFBitmap_FillRect`, `FPDF_RenderPageBitmapWithMatrix` |
| Progressive | `FPDF_RenderPageBitmap_Start`, `FPDF_RenderPage_Continue`, `FPDF_RenderPage_Close` |
| Text | `FPDFText_LoadPage`, `FPDFText_CountChars`, `FPDFText_GetCharBox`, `FPDFText_GetText` |
| Links | `FPDFLink_LoadWebLinks`, `FPDFLink_GetLinkAtPoint` |
| Outline | `FPDFBookmark_GetFirstChild`, `_GetNextSibling`, `_GetTitle`, `_GetDest` |
| Annots (read) | `FPDFPage_GetAnnotCount`, `FPDFPage_GetAnnot`, `FPDFAnnot_GetSubtype`, `_GetRect` |
| Forms | `FPDFDOC_InitFormFillEnvironment`, `FORM_OnAfterLoadPage`, `FORM_OnLButtonDown`, `FPDF_FFLDraw` |

`FPDFBitmap_CreateEx` with an external buffer is important: it lets PDFium render **directly into your persistently-mapped GPU staging memory**, eliminating one full-tile copy per tile. At 60 fps with a dozen tiles in flight that is real bandwidth.

### PoDoFo calls, by feature

| Feature | Calls |
|---|---|
| Load | `PdfMemDocument::Load(path\|buffer, password)` |
| Pages | `GetPages()`, `CreatePage`, `RemovePageAt`, `InsertDocumentPageAt` |
| Rotation | page dictionary `/Rotate` via `GetDictionary().AddKey` |
| Annots | `PdfPage::CreateAnnotation`, `PdfAnnotation::SetAppearanceStream` |
| Fonts | `GetFonts().SearchFont`, auto-subsetting on save |
| Content | `PdfPainter` for watermark / header-footer / Bates injection |
| Metadata | `GetMetadata()`, XMP via `GetOrCreateXmpPacket` |
| Encryption | `SetEncrypted(PdfEncryptAlgorithm::AESV3R6, ...)` |
| Signatures | `PdfSignerCms`, `PoDoFo::SignDocument` (deferred-signing variant) |
| **Save** | `SaveUpdate(device, PdfSaveOptions::...)` ← the snapshot engine |
| Full save | `Save(device, PdfSaveOptions::NoFlateCompress \| ...)` |

> **Note on `SaveUpdate`:** PoDoFo writes only the incremental delta; it is the caller's job to ensure the output device already contains the original bytes. That is exactly what `DocumentSnapshot`'s rope gives you, which is why the two designs fit together so naturally.

---

<a name="appendix-c-skia"></a>
## Appendix C — The Skia route: what actually varies per platform

If you want to evaluate owning the UI layer, here is the concrete scope. The headline: **roughly 85% of the code is platform-agnostic.** Skia draws identically everywhere — that is its entire purpose. The cost is concentrated in eight specific places, and the right dependency choices collapse most of them.

### C.1 What does *not* change

Everything below is written once and compiled for all six targets:

- All layout — flexbox/stack solver, constraints, measurement
- All painting — every one of the 26 primitives, drawn with `SkCanvas`
- All animation — the 38 motions in §10, driven by one clock
- Hit-testing, focus traversal, keyboard model, gesture recognition
- The entire application core, render service and PDF stack (§4)
- **Shaders** — SkSL compiles to GLSL/MSL/HLSL/SPIR-V/WGSL from one source

That last point matters more than it looks. The circular reveal (§10.3) and the page-theme shader (§9.1) become a single `SkRuntimeEffect` each, written once. No per-backend shader variants, no `.qsb` pipeline.

### C.2 The eight things that vary

| # | Concern | Varies by platform | Mitigation |
|---|---|---|---|
| 1 | Window + surface creation | Heavily | **SDL3** — one API, all six targets |
| 2 | GPU context binding | Heavily | **Graphite + Dawn** — see C.4 |
| 3 | Event loop, input, DPI, vsync | Heavily | SDL3 |
| 4 | Font enumeration + system fallback | `SkFontMgr` per OS | Ship your own font set — see C.6 |
| 5 | **Text shaping, bidi, line breaking** | Not platform-specific, but *missing* | HarfBuzz + SheenBidi + libunibreak |
| 6 | **IME / composition** | Heavily, and painfully | SDL3 covers ~80%; candidate-window placement is manual |
| 7 | **Accessibility** | Completely | AccessKit for Win/mac/Linux/Android; **iOS and web are manual** |
| 8 | File dialogs, DnD, clipboard, printing | Heavily | SDL3 covers the first three; **printing is yours** |

Items 5–8 are the iceberg. Items 1–3 look scary and are mostly solved by one dependency.

### C.3 Recommended stack

| Layer | Choice | Licence |
|---|---|---|
| Rasteriser | **Skia**, Graphite backend | BSD-3 |
| GPU abstraction | **Dawn** (WebGPU) | BSD-3 |
| Window / input / lifecycle | **SDL3** | Zlib |
| Shaping | **HarfBuzz** | MIT |
| Bidi | **SheenBidi** | MIT — ~100 KB vs ICU's tens of MB |
| Line breaking | **libunibreak** | Zlib |
| Accessibility | **AccessKit** (C bindings) | MIT / Apache-2.0 |
| PDF | PDFium + PoDoFo | BSD-3 / MPL-2.0 |

> **Every dependency is permissive. Zero licence cost, zero copyleft exposure, on all six platforms — including iOS and web.** This is the Skia route's strongest argument and the direct answer to the Qt licensing problem in §2.2.

### C.4 The surface matrix — and the unified option

The conventional Ganesh approach needs a different backend per platform:

| Platform | Ganesh backend | Surface |
|---|---|---|
| Windows | D3D12 or Vulkan | `GrDirectContext::MakeDirect3D` + swapchain |
| macOS | Metal | `CAMetalLayer` |
| Linux | Vulkan (GL fallback) | `VkSurfaceKHR` on Wayland/X11 |
| iOS | Metal | `CAMetalLayer` |
| Android | Vulkan or GLES | `ANativeWindow` → `VkSurfaceKHR` / EGL |
| WASM | WebGL2 | emscripten GL context |

That is six context-creation paths and six sets of driver quirks.

**Graphite + Dawn collapses this to one.** Skia's new Graphite backend targets Metal, Vulkan and Dawn, and Dawn is Google's WebGPU implementation that wraps D3D12, Vulkan and Metal underneath. Graphite reached Chrome Stable on Apple Silicon in July 2025 and is default on Chrome for Mac as of December 2025, with Windows progressing through Dawn and Android functional on Vulkan 1.1 devices. Google is actively deprecating Ganesh — Dawn support was removed from Ganesh in m118 and is Graphite-only.

So the forward-looking configuration is:

```
            ┌──────────────────────────────┐
            │   Folio UI  (one codebase)   │
            ├──────────────────────────────┤
            │   Skia Graphite              │
            ├──────────────────────────────┤
            │   Dawn (WebGPU)              │
            ├──────┬──────┬──────┬─────────┤
            │D3D12 │Metal │Vulkan│ WebGPU  │
            │ Win  │mac/iOS│Lin/And│  web   │
            └──────┴──────┴──────┴─────────┘
```

**One backend, one shader language, six platforms** — which is precisely the "identical everywhere" property you asked for, achieved by construction rather than by testing.

Two caveats to verify in the spike rather than assume:

- **Graphite does not support OpenGL or D3D11 directly.** Dawn provides D3D12 on Windows and Vulkan on Linux; machines without a working modern-API driver need a Ganesh fallback path, exactly as Chrome keeps one.
- **Skia's WGSL shader output for in-browser WebGPU** was historically behind its SPIR-V path (which works with dawn-native but not browser WebGPU). Confirm the WASM target compiles and runs before committing.

Pragmatic recommendation: **build against Graphite, keep a Ganesh/GL path behind a flag** for old drivers and as a WASM fallback to WebGL2.

### C.5 The prize: zero-copy PDFium

This is the architectural payoff that only exists on the Skia route.

PDFium has a Skia rendering backend (`pdf_use_skia=true`). If your application *and* PDFium share the same Skia, PDFium can draw page content **directly into your canvas** — no intermediate `FPDF_BITMAP`, no CPU copy, no texture upload.

| | Qt / Ganesh-bitmap path | Shared-Skia path |
|---|---|---|
| PDFium renders to | `FPDFBitmap` in system RAM | your `SkCanvas` |
| CPU copy | 1 per tile | 0 |
| Texture upload | 1 per tile | 0 |
| §8.5 staging-buffer machinery | required | **deleted** |

For a 4K display scrolling a dense document that removes a meaningful slice of the per-frame budget and simplifies §8 considerably.

**The catch, and it is a real one:** Skia has **no stable API or ABI**. It is a Chromium monorepo dependency that changes continuously. PDFium pins a specific Skia revision. To get zero-copy you must build PDFium and your application's Skia **from the same checkout at the same revision**, and re-validate on every uplift. PDFium's Skia backend has also historically been the less-travelled path versus the default AGG rasteriser.

Mitigation: one GN/ninja pipeline that produces PDFium + Skia + Dawn as a matched artefact set, versioned together and published to your package registry. You need this pipeline for PDFium anyway (§15.2), so the marginal cost is low — but treat the uplift cadence as quarterly, not continuous.

### C.6 The text iceberg

Skia rasterises glyphs. It does not do shaping, bidi, line breaking, or font fallback. You assemble that:

```
  text run
     │
     ▼  SheenBidi        → bidi levels, RTL/LTR runs
     ▼  libunibreak      → line break opportunities (UAX #14)
     ▼  font fallback    → split runs by coverage
     ▼  HarfBuzz         → shape each run → glyph IDs + positions
     ▼  SkTextBlob       → Skia rasterises
```

**Do not pull in full ICU.** It is tens of megabytes of data and would dominate your WASM budget. SheenBidi plus libunibreak cover the two things you actually need in ~150 KB combined.

**Font fallback is where pixel-identity is won or lost.** Using each OS's `SkFontMgr` (DirectWrite / CoreText / fontconfig) guarantees the UI renders *differently* on every platform — the exact failure you are trying to avoid. Instead:

- Ship **DM Sans** for Latin UI text.
- Ship a curated **Noto subset chain** for the scripts you support — Bengali, Devanagari, Arabic, CJK as needed. Subset aggressively; a Bengali Noto subset is a few hundred KB, full CJK is several MB and should be lazily fetched on web.
- Use the system `SkFontMgr` **only as a last-resort fallback**, and log when it fires — every hit is a potential cross-platform diff.

PDF *content* fonts are PDFium's problem, not yours, and PDFium already has its own font handling. This applies to chrome text only.

### C.7 IME and accessibility — the honest cost

**IME.** SDL3's `SDL_StartTextInput` / `SDL_EVENT_TEXT_EDITING` handle composition on most platforms and get you a long way. What SDL does not fully solve is **candidate-window positioning** — the IME needs to know where the caret is on screen, and getting that right for Chinese, Japanese and Korean input requires per-platform work (`ITfContextOwner` on Windows, `NSTextInputClient` on macOS, `text-input-v3` on Wayland). Budget this properly; a PDF editor with broken CJK input is unusable in three of the world's largest markets.

**Accessibility.** AccessKit is the right answer and ships C bindings with a documented SDL example. Coverage:

| Platform | AccessKit | Notes |
|---|---|---|
| Windows | ✅ UI Automation | released |
| macOS | ✅ NSAccessibility | released |
| Linux | ✅ AT-SPI | released |
| Android | ⚠️ limited | GameActivity only |
| **iOS** | ❌ none | **manual `UIAccessibility`** |
| **Web** | ❌ none | **manual ARIA shadow DOM** |

So two of your six targets need hand-written accessibility. This is the single strongest argument for Qt, which ships all six. If accessibility is a procurement requirement — government, education, enterprise — weigh this heavily.

### C.8 Build and pinning

- **Skia, Dawn and PDFium all use GN + ninja**, not CMake. One pipeline builds all three from a pinned `DEPS`; your application stays CMake and consumes the artefacts.
- Pin a Skia **milestone branch** (e.g. `chrome/m1xx`), never `main`. Uplift deliberately, on a schedule.
- Vendor the pinned revision. Skia's lack of API stability means a drive-by `git pull` can cost a week.
- WASM: Skia builds for Emscripten (this is what CanvasKit is); expect to carry a few patches.

### C.9 Effort delta and the verdict

Against the Qt baseline in §19, going Skia adds roughly:

| Work item | Added effort |
|---|---|
| Widget/layout/animation framework | ~16 weeks |
| Text stack assembly + font strategy | ~10 weeks |
| IME beyond what SDL3 gives | ~8 weeks |
| Accessibility (AccessKit integration + iOS/web by hand) | ~12 weeks |
| Printing, native dialogs, platform polish | ~6 weeks |
| Skia/Dawn/PDFium build pipeline + uplift tax | ~6 weeks |
| **Total** | **~58 engineer-weeks** |

Partially parallelisable, so call it **+6 to +9 months** on the programme — landing at roughly 24 months rather than 16–18.

**Choose Skia if:** the commercial Qt licence is unacceptable *and* you are building a long-lived product platform rather than shipping one app; pixel-identity is contractual; and accessibility on iOS/web is either not required or you accept writing it.

**Choose Qt if:** you need to ship inside 18 months, accessibility is a requirement on all six platforms, or the team is five people rather than eight.

**The zero-copy PDFium integration and the uniformly permissive licensing are genuinely compelling** — more so than the generic "custom engine" pitch. If the Phase 0 spike shows Graphite + Dawn working cleanly on all six targets, this route deserves a serious second look rather than the drawer.



1. **Pick the toolkit with eyes open, in Phase 0, after a real spike on real hardware** — including a WASM build and an iOS device. The Qt-WASM licensing finding means this is a commercial decision as much as a technical one.
2. **Keep the core toolkit-agnostic from commit one.** A grep-based CI rule costs an hour and buys you the ability to change your mind in year two.
3. **Never let the two engines both think they own the document.** The snapshot protocol in §5 and the authority state machine in §5.4 are not ceremony — they are the difference between a PDF editor and a PDF corrupter.

The PDF part of this project is well-understood and has good libraries. The hard, differentiating work is the 38 animations, the six platform shells, and the discipline to keep them identical. Budget accordingly.

---

<a name="appendix-d-validation"></a>

## Appendix D — What was built and measured on Linux (2026-10-08)

Everything above this appendix was a design document. This appendix is the
part that was compiled and run. Where the two disagree, this one is right.

See `STATUS.md` for the full numbers and `tools/fetch-deps.sh` for the exact
pins. Reproduce with:

```bash
./tools/fetch-deps.sh && cmake -S . -B build -G Ninja -DFOLIO_BUILD_UI=OFF \
  && cmake --build build && ctest --test-dir build --output-on-failure
```

### D.1 What now exists and passes

| Layer | Status |
|---|---|
| `folio-core` — snapshot rope, tile geometry, raster cache, scheduler, utilities | builds and passes with **zero third-party dependencies** |
| `folio-engine-pdfium` — library init, document load, tiled + progressive raster | builds and passes against **PDFium 157.0.8086** |
| `folio-render` — backend-neutral `IPaintTarget` | builds |
| `folio-render-skia` — paint target, tile compositor, page-theme SkSL | builds and passes against **Skia m144** |
| end-to-end | real PDF → PDFium → cache → Skia → PNG, asserted per theme |

**401 assertions, 7 ctest suites, 0 failures, 0 warnings** under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

### D.2 The three findings that change the plan

**1. Tiling needs bleed.** See §8.1.1. The B3 acceptance criterion was wrong;
it has been rewritten and the fix is implemented and tested.

**2. No public Skia prebuilt ships Graphite.** `JetBrains/skia-pack` m144 has
the full `include/gpu/graphite/` header tree, including the Dawn backend —
and **zero `skgpu::graphite::*` symbols** in `libskia.a` (against 229 for
`GrDirectContext`). Appendix C's central claim, *Graphite + Dawn = one backend
across six platforms*, therefore carries a mandatory "build Skia yourself from
GN" prerequisite: depot_tools, a ~20 GB checkout, and a pinned-revision uplift
cadence. That cost was already in Appendix C's +6-to-9-month estimate. It is
now confirmed rather than assumed, and `FolioDependencies.cmake` detects and
reports it so it cannot be discovered at link time.

**3. The Skia route is cheaper to *start* than Appendix C implied.** A working
Skia backend — paint target, tile compositor, runtime-shader page theming,
PNG golden output — took one session against a prebuilt. Linking is seconds,
not minutes. A minimal Skia binary is **6.2 MB**; the full test binary with
PDFium is 8.6 MB. The expensive parts of Appendix C (text shaping, IME,
accessibility, window/event plumbing) are all still ahead, and
**AccessKit still has no iOS and no web adapter** — that remains the
dealbreaker. But "Skia is a research project" is no longer a fair description
of the risk.

### D.3 The decision, restated

Nothing here overturns §2.2. It sharpens it:

- `folio-core` is **provably** toolkit-free — a CI rule enforces it, and it
  has already caught a violation.
- A `folio-render/IPaintTarget` seam now exists, with a **working Skia
  implementation behind it**. The Qt-versus-Skia choice is no longer a
  one-way door; it is a directory swap plus the platform-shell work.
- The page theme is a **shader uniform**, demonstrated: one rasterised tile
  composites to light, sepia and dark with no re-render. That property is
  independent of toolkit and is now load-bearing in the memory budget.

> **Recommendation unchanged.** Qt Quick remains the primary if a commercial
> licence is affordable, because it solves iOS and web accessibility, IME and
> text shaping — the four things Appendix C cannot. Skia + SDL3 remains the
> answer when the licence budget is zero. What has changed is that the second
> option has been partially built and measured rather than merely costed, and
> the migration path between them is now a real seam in the code rather than
> a paragraph in a plan.

### D.4 Performance, with the caveat that this is a CPU

Two-core sandbox, software raster, no GPU. Treat as an upper bound on cost,
not as frame budget.

| stage | 612×792 at ×1 |
|---|---|
| PDFium raster, 4 tiles with bleed | 1–4 ms |
| Skia composite, light (no colour filter) | 8.4 ms |
| Skia composite, sepia/dark (runtime shader) | 30.4 / 30.9 ms |

The 3.6× light-versus-themed gap is why `PageTheme::isIdentity()` returns
`nullptr` rather than an identity filter: skipping the pass entirely matters
on the WASM software path and on low-end Android, and costs nothing to keep.
