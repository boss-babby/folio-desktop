# Folio — Full Implementation Plan
### 78 work packages · dependency-ordered · AI-assisted execution model

> Companion to `Folio-Native-Architecture-Plan.md`. That document decides *what* and *why*. This one is *how*, *in what order*, and *how you know each piece is done*.

---

## Progress — 2026-10-08

**13 of 78 work packages complete, compiled and tested on Linux against real
PDFium 157 and real Skia m144.** 401 assertions, 7 ctest suites, 0 failures,
0 warnings. Full detail and measured numbers in `STATUS.md`.

```
A1 ███ A2 ███ A4 ███ A8 ███      foundations, deps, utilities, boundary guards
B1 ███ B2 ███ B3 ███ B4 ███      snapshot rope, PDFium load, tiled + sliced raster
C1 ███ C2 ███ C4 ███             tile geometry, raster cache, scheduler
R1 ███ R2 ███                    paint interface + Skia backend  (NEW, see below)
```

**Three work packages added** that were not in the original 78, because the
Qt-versus-Skia decision turned out to be worth making reversible in code
rather than in prose:

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **R1** | `folio-render/include/folio/render/IPaintTarget.h` — backend-neutral drawing vocabulary; `PageTheme.h` | A3 | every UI primitive in the prototype is expressible; nothing in the header names a toolkit |
| **R2** | `folio-render/skia/` — `SkiaPaintTarget`, `SkiaTileCompositor`, `PageThemeShader` (SkSL) | R1, C2 | a cached tile composites to light/sepia/dark from **one** raster; golden PNG written headlessly |
| **R3** | `folio-render/qt/` — the same interface over `QSGNode`/`QPainter` | R1 | the same golden images match the Skia backend within ΔE2000 < 1.0 — *not started, needs Qt* |

**Two acceptance criteria were rewritten after contact with the real library:**

- **B3** — "byte-identical" → *byte-identical at scale ≤ 2.0, worst channel
  delta ≤ 1 above it*, and tiles must be rendered with a bleed margin. See
  architecture plan §8.1.1.
- **C4** — tile coordinates are carried on the request, not derived from the
  device rect. The derivation silently broke the moment bleed existed.

**Revised near-term order.** B6/B7/B8 (PoDoFo) are the next critical-path
items and are the only thing still blocked on an external dependency;
`tools/fetch-deps.sh` builds PoDoFo 1.1.0 from source but that path has not
been exercised yet. C3/C5/C6 (texture cache, render service, prefetcher) are
unblocked and buildable today.

---

## 0. Calibration

The architecture plan estimated 16–18 months for a team of five. That assumed humans writing every line.

**What AI assistance genuinely compresses:**
- Interface and boilerplate authoring — the 26 UI components, bindings, build files
- Known-pattern implementations — LRU caches, tile schedulers, state machines, command journals
- Test scaffolding, golden harnesses, CI configuration
- Documentation and code review passes

**What it does not compress:**

| Irreducible | Why |
|---|---|
| PDFium GN build across 6 toolchains | Toolchain archaeology; each platform fails differently |
| Emscripten ports for PoDoFo's dependency tree | libxml2/OpenSSL under WASM is a known slog |
| GPU driver quirks | AMD/Intel/NVIDIA/Mali/Adreno/Apple each misbehave uniquely |
| iOS provisioning, signing, App Store review | Days of latency *per cycle*, and you will have cycles |
| Real-device test matrix | You cannot emulate a 3-year-old Android phone's thermal throttling |
| IME validation | Requires actually typing Japanese, Korean, Chinese, Bengali |
| PDF corpus triage | Finding which 200 of 10,000 PDFs render wrong is empirical |
| Screen-reader validation | NVDA/VoiceOver/TalkBack behave differently than their specs |

**Revised estimate: 5–7 months, 1–2 engineers driving.** The engineer's job shifts from authoring to *integrating, validating and deciding*. That is still a real job, and it is still the bottleneck — but it is a 3× compression, not a 50× one.

### Milestone ladder

| When | State |
|---|---|
| **Week 1** | Repo builds on 3 desktop platforms; PDFium + PoDoFo linked; a PDF page renders to a window |
| **Week 3** | Read mode usable: scroll, zoom, thumbnails, outline, two themes, 60 fps |
| **Week 6** | Full chrome: all 5 modes switchable, all panels, all 38 animations |
| **Week 10** | Annotate + Organize functional, saving correctly through PoDoFo |
| **Week 14** | Edit + Forms; desktop feature-complete |
| **Week 18** | iOS + Android |
| **Week 22** | WASM |
| **Week 26** | Hardened: a11y, l10n, fuzzing, installers, signed and shippable |

---

## 1. Execution model

Work packages are sized to **one focused session each** — roughly 0.5–3 days of wall-clock including review, test and integration. Each WP states:

- **Produces** — the files that exist afterwards
- **Depends** — WPs that must land first
- **Done when** — an objective, checkable condition

Rules:
1. **No WP merges without its acceptance condition demonstrably met.** Not "looks right" — the stated check.
2. **Blocks A–D have no UI dependency** and are validated by unit tests alone. Build them first; they are the foundation and the easiest to get wrong silently.
3. **Golden images are generated the moment a component exists**, not retrofitted at the end.
4. **Perf gates run from WP C6 onward.** Regressions are caught the week they appear, not in month five.

---

## 2. Complete repository tree

This is the full target state. Every WP below adds files to this tree.

```
folio/
├── CMakeLists.txt
├── CMakePresets.json                  12 configs: 6 platforms × Debug/Release
├── vcpkg.json                         manifest mode
├── design-tokens.json                 ← single source of truth
├── .github/workflows/
│   ├── build.yml                      6-platform matrix
│   ├── golden.yml                     cross-platform pixel diff
│   ├── perf.yml                       dedicated runner, frame-time gates
│   └── fuzz.yml                       continuous
├── cmake/
│   ├── FolioOptions.cmake             feature flags, FOLIO_WASM_MINIMAL
│   ├── FolioDependencies.cmake        pdfium/podofo discovery
│   ├── FolioGolden.cmake              golden-test registration
│   ├── FolioCoreGuard.cmake           CI rule: no Qt types in folio-core
│   └── toolchains/
│       ├── ios.cmake  android.cmake  wasm.cmake
├── third_party/
│   ├── pdfium/                        prebuilt artefacts per platform
│   ├── podofo/                        vcpkg or submodule
│   └── fonts/                         DM Sans + Noto subsets
│
├── tools/
│   ├── token-gen/                     design-tokens.json → Tokens.qml + .h
│   ├── icon-atlas/                    60 SVG paths → SDF atlas
│   ├── golden-runner/                 headless capture + perceptual diff
│   └── corpus-runner/                 PDF regression harness
│
├── folio-core/                        PORTABLE C++20 — zero Qt
│   ├── include/folio/core/
│   │   ├── util/      Observable.h SlotMap.h Arena.h Result.h Trace.h
│   │   ├── model/     Document.h Page.h Annotation.h FormField.h Geometry.h
│   │   ├── engine/    IPdfRasterizer.h IPdfDocumentStore.h IPdfTextIndex.h
│   │   │              DocumentSnapshot.h EditGuard.h
│   │   ├── render/    TileKey.h TileGrid.h RasterCache.h LodPolicy.h
│   │   │              TileScheduler.h PrefetchPredictor.h OverlayScene.h
│   │   ├── session/   DocumentSession.h SessionManager.h EditJournal.h
│   │   │              ModeController.h Selection.h LayerVisibility.h
│   │   ├── text/      TextIndex.h SearchService.h Normalize.h
│   │   └── platform/  IFileSystem.h IPreferences.h IClipboard.h ITrace.h
│   └── src/           …mirrored implementations
│
├── folio-engine/                      PDFium + PoDoFo adapters
│   ├── pdfium/        PdfiumLibrary.cpp PdfiumRasterizer.cpp
│   │                  PdfiumTextIndex.cpp PdfiumForms.cpp PdfiumPause.cpp
│   └── podofo/        PodofoStore.cpp PodofoPageOps.cpp PodofoAnnots.cpp
│                      PodofoAppearance.cpp PodofoSave.cpp
│
├── folio-ui/
│   ├── kit/           26 .qml primitives + Tokens.qml (generated)
│   ├── screens/       Welcome/ Window/ panels/ popovers/
│   ├── viewport/      FolioPageView.{h,cpp} TileNode.cpp TexturePool.cpp
│   │                  OverlayNode.cpp GestureController.cpp
│   ├── shaders/       circular_reveal.frag page_theme.frag sdf_icon.frag
│   └── bindings/      Q_OBJECT adapters over folio-core
│
├── folio-shell/
│   ├── windows/  macos/  linux/  ios/  android/  wasm/
│
└── tests/
    ├── core/  engine/  render/  golden/  corpus/  perf/  fuzz/
```

---

## 3. Work packages

### Block A — Foundations (8 WPs · ~1.5 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **A1** | `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, repo tree, `.gitignore` | — | `cmake --preset desktop-debug && cmake --build` succeeds empty on Win/mac/Linux |
| **A2** | `FolioDependencies.cmake`; PDFium prebuilts vendored; PoDoFo 1.0 via vcpkg | A1 | A test binary calls `FPDF_InitLibrary()` and `PdfMemDocument{}` and exits 0 on 3 platforms |
| **A3** | `tools/token-gen`, generated `Tokens.qml` + `folio_tokens.h` | A1 | Changing a hex in `design-tokens.json` changes both outputs on rebuild |
| **A4** | `util/`: `Observable<T>`, `SlotMap`, `Arena`, `Result`, `Trace` | A1 | Unit tests pass; `Observable` has no allocation on notify |
| **A5** | `platform/` interfaces + desktop impls (fs, prefs, clipboard) | A4 | Round-trip write/read a pref on 3 platforms |
| **A6** | GoogleTest wiring, `ctest` integration, `tests/core` skeleton | A1 | `ctest` green with ≥1 real test |
| **A7** | `.github/workflows/build.yml` — 6-platform matrix | A1,A2 | All 6 configure+build green (may be empty targets) |
| **A8** | `FolioCoreGuard.cmake` — fails build if `folio-core/` includes `<Q…>` | A1 | Deliberately adding `#include <QString>` fails CI |

> **A7 is the week-one risk.** iOS/Android/WASM toolchains will fight you. Get them green while the codebase is trivial — debugging a toolchain and a codebase simultaneously is twice as hard.

### Block B — Engine layer (8 WPs · ~2 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **B1** | `DocumentSnapshot` — rope buffer, generations, `dirtyPagesSince()` | A4 | Append 3 deltas; `read()` at any offset matches a flat concatenation; fuzz 10k random reads |
| **B2** | `PdfiumLibrary` (init/teardown, thread affinity assert), `PdfiumRasterizer::setSnapshot` via `FPDF_LoadCustomDocument` | A2,B1 | Loads a PDF through the rope with zero temp files; page count correct |
| **B3** | Tile render — `FPDF_RenderPageBitmapWithMatrix`, `FPDFBitmap_CreateEx` into caller memory, BGRx | B2 | A 4×4 tile grid reassembles byte-identical to one full-page render |
| **B4** | `PdfiumPause` — `IFSDK_PAUSE` cooperative scheduler, 4 ms slices | B3 | A 2 s page render yields ≥100 times; a high-priority tile preempts within 8 ms |
| **B5** | `PdfiumTextIndex` — `FPDFText_*`, char boxes, Unicode normalisation | B2 | Extracted text matches reference for 20-doc corpus; char boxes align to rendered glyphs within 1 px |
| **B6** | `PodofoStore` — load, object model, `EditGuard` state machine | A2,B1 | `beginEdit()` returns `EditRejected` while in `PDFIUM_FORM_OWNS`; cannot be bypassed |
| **B7** | `SaveUpdate` → new snapshot; dirty-page computation | B1,B6 | Rotate page 3 → `dirtyPagesSince()` returns exactly `{3}` |
| **B8** | **Round-trip fidelity test** | B2,B7 | Open → save with no edits → output byte-identical to input. **Non-negotiable gate.** |

> **B8 is the most valuable test in the project.** It catches the silent-corruption class of bug — user opens a contract, changes nothing, saves, loses a signature field. Write it early and never let it go red.

### Block C — Render service (6 WPs · ~1.5 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **C1** | `TileKey` (16 B), `TileGrid`, page↔device coordinate math | A4 | Property test: round-trip any point through all rotations/zooms |
| **C2** | `RasterCache` — LRU, byte budget, `distance×age/bytes` scoring | C1 | Under a 64 MB cap with 500 pages, resident set tracks viewport; no unbounded growth over 10k ops |
| **C3** | `LodPolicy` — proxy / screen / detail | C1,C2 | Proxy for every page resident at ≤256 KB each; never evicted |
| **C4** | `TileScheduler` — 6 priority lanes, cancellation | B4,C2 | Visible tiles always complete before thumbnails under saturation |
| **C5** | `PrefetchPredictor` — velocity-weighted band | C4 | Simulated 2000 px/s scroll: ≥90% of pages have a tile ready on arrival |
| **C6** | **Perf harness** — synthetic scroll/zoom benchmark, p99 frame time | C4,C5 | Reports p99; wired into `perf.yml`; baseline recorded |

### Block D — Application core (8 WPs · ~2 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **D1** | `Document`, `Page`, `Annotation`, `FormField`, `Geometry` | A4 | Serialise→deserialise round-trips |
| **D2** | `DocumentSession` — wires store + rasterizer + caches | B7,C4 | Open a doc, request tiles, apply an edit, observe correct invalidation |
| **D3** | `EditJournal` — typed invertible commands, undo/redo | D2 | Property test: any 100-command sequence, full undo restores initial snapshot hash |
| **D4** | `ModeController` — 5 modes, tool sets, `Ctrl+1..5` | D2 | Mode change emits exactly one notification; tool defaults match the prototype |
| **D5** | `Selection` + `TextSelection` (quads from char boxes) | B5 | Drag-select across a line returns contiguous quads matching the prototype's visual |
| **D6** | `SearchService` — normalised index, match navigation | B5 | "3 of 17" counts correct incl. ligatures and hyphen-joins on corpus |
| **D7** | `OutlineModel`, `NotesModel`, `LayerVisibility` | B2,D1 | Outline tree matches `FPDFBookmark_*` traversal; 4 layer toggles independent |
| **D8** | `SessionManager` (tabs), `RecentFiles`, `Preferences` | D2,A5 | Open 3 docs, close middle, indices stay correct; recents persist across restart |

### Block E — UI kit (10 WPs · ~2.5 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **E1** | Qt project wiring, `Tokens` singleton, light/dark switch | A3 | Toggling theme recolours a test window; zero hard-coded hex in `folio-ui/` |
| **E2** | `Button`, `IconButton`, `ToggleButton`, `SegmentedControl`, `Divider`, `SectionHeader` | E1 | All 5 states each; `:active scale(.93)` at 150 ms matches prototype |
| **E3** | `TextField`, `NumberField`, `Select`, `Slider`, `Switch`, `ColorSwatch` | E1 | `Switch` knob travels 14 px in 300 ms; `ColorSwatch` ring at 200 ms |
| **E4** | `Card`, `Panel`, `CollapsibleRail`, `Tab`, `TabStrip` | E1 | Tab opens via width 0→178 px in 350 ms; close splices after 300 ms |
| **E5** | `Popover`, `Menu`, `MenuItem`, `Tooltip`, `Toast`, `ProgressBar`, `DropZone` | E1,E7 | Tooltip appears after exactly 380 ms hover; toast auto-dismisses at 1700 ms |
| **E6** | `tools/icon-atlas` — 60 SVG paths → runtime SDF atlas | E1 | All icons from **one** material; verified via `QSG_RENDERER_DEBUG=render` |
| **E7** | `CircularReveal` ShaderEffect | E1 | Radius formula matches `hypot(max(ox,w-ox),max(oy,h-oy))+8`; origin = trigger centre |
| **E8** | Theme transition — snapshot + animated mask, 760 ms | E7 | Visually matches the prototype's View-Transition reveal; direction flips light↔dark |
| **E9** | Golden harness: 26 components × 2 themes = 52 images | E2–E5,A6 | All 52 captured; cross-platform diff ΔE2000 < 1.0 on Win/mac/Linux |
| **E10** | **Animation audit** — all 38 motions from §10 | E2–E8 | Deterministic clock; each motion asserted at its midpoint frame |

> **E10 is how you prevent drift.** With a frozen animation clock you can assert the exact frame at t=170 ms of the panel reveal — that catches easing regressions a settled-state screenshot never would.

### Block F — Viewport (6 WPs · ~2 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **F1** | `FolioPageView` — `QQuickItem` + `QSGNode` tile rendering | C4,E1 | A real PDF page renders; no `QQuickPaintedItem` anywhere |
| **F2** | `TexturePool` + budgeted uploader (≤2 ms/frame) | F1 | Uploading 20 tiles costs ≥10 frames, drops none |
| **F3** | Gesture controller — scroll, pinch, wheel, GPU pre-transform | F1 | **Zero re-raster during gesture**; re-raster fires 120 ms after settle |
| **F4** | `OverlayNode` — vector annotations, live ink | F1 | Pen stroke has zero engine latency; appends per input event |
| **F5** | Page-theme fragment shader | F1 | Sepia/dark cross-fade in 400 ms with no re-raster |
| **F6** | Three layouts: continuous, single, two-up | F1,D2 | Each matches the prototype's scroll/paging semantics |

### Block G — Screens (8 WPs · ~2 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **G1** | Window shell: title bar, tab strip, command bar | E2–E5,D8 | Tabs open/close/switch with correct animation; tool strip swaps at 160 ms |
| **G2** | Left panel: Pages / Outline / Notes / Layers | E4,D7 | Pane switch 300 ms + 8 px rise; collapse to 48 px rail with circular reveal |
| **G3** | Right panel: 5 mode-dependent panels | E3,D4 | Panel swap 300 ms + 12 px slide; contents match prototype per mode |
| **G4** | Bottom pill + find / display / dynamic popovers | E5,D6 | Find shows live "N of M"; popover reveals from trigger centre at 460 ms |
| **G5** | Welcome screen: hero, drop zone, recents grid/list, search | E2–E5,D8 | Staggered entrance at 80+i×50 ms; greeting is time-of-day aware |
| **G6** | Focus / full-screen mode | G1 | Title + command bar translate −130% over 500 ms; `#work` top 112→12 |
| **G7** | Windows chrome: `WM_NCCALCSIZE`, `WM_NCHITTEST`, DWM shadow | G1 | **Win11 Snap Layouts work** (`HTMAXBUTTON` on hover); per-monitor DPI v2 |
| **G8** | macOS + Linux chrome | G1 | macOS traffic lights positioned per §12.1 decision; Wayland + X11 CSD resize |

### Block H — Modes (8 WPs · ~3 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **H1** | Read mode complete | F6,G1–G4 | All read-path perf gates green; page themes, layouts, outline, find |
| **H2** | Annotate: 6 tools, overlay commit, `/AP` generation | F4,B6,D3 | Annotations open correctly in **Acrobat, Preview and Chrome** |
| **H3** | Organize: grid, multi-select, drag-reorder, page ops | E4,B7,D3 | B8 round-trip stays green after every operation |
| **H4** | Edit: text edit in place, add text, image, link, shape, redact | B6,D3 | Redaction **destroys** content, not just covers it — verify with `qpdf --qdf` |
| **H5** | Edit: crop, header/footer, watermark, background, Bates | B6 | Content-stream injection preserves existing `/Contents` arrays |
| **H6** | Forms: placement, fill session, authority handoff | B6,B2 | State machine has no escape path; filled values survive save→reload |
| **H7** | Undo/redo wired across all modes | D3,H2–H6 | 200 mixed operations, full undo → initial snapshot hash |
| **H8** | Save / export pipeline (PDF, flatten, image, text) | B7 | Incremental save; linearised save; flatten removes interactivity correctly |

### Block I — Platforms (8 WPs · ~4 weeks, latency-bound)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **I1** | iOS shell + adaptive layout (<768, 768–1024) | G1–G6 | Runs on a **physical device**; safe-area insets correct; 60 fps scroll |
| **I2** | Android shell | G1–G6 | Runs on a physical mid-range device; back-gesture handled |
| **I3** | Touch + stylus input; Pencil/S-Pen pressure → stroke width | I1,I2,F4 | Palm rejection works; pressure curve matches |
| **I4** | WASM build + cross-origin isolation | A7 | COOP/COEP served; pthreads active; Asyncify fallback verified |
| **I5** | WASM storage: IDBFS, File System Access API | I4 | Recents persist across reload; explicit sync point flushes |
| **I6** | Printing per platform | H8 | Correct output on a physical printer on Win/mac/Linux |
| **I7** | Native file dialogs, DnD, clipboard per platform | A5 | Drag a PDF onto the dock/taskbar icon opens it |
| **I8** | Installers, code signing, notarisation, store packaging | all | Signed artefacts install cleanly on fresh VMs/devices |

> **Block I is latency-bound, not effort-bound.** Apple notarisation, App Store review and Play Console review each add days you cannot compress. Start I8 paperwork in week 1, not week 20.

### Block J — Hardening (8 WPs · ~3 weeks)

| WP | Produces | Depends | Done when |
|---|---|---|---|
| **J1** | Accessibility: roles, labels, focus order, keyboard nav | G1–G6 | NVDA, VoiceOver and TalkBack each complete a full read→annotate→save flow |
| **J2** | Localisation + RTL mirroring | E1–E5 | Arabic/Hebrew mirror correctly; no clipped strings at 1.4× expansion |
| **J3** | PDF corpus regression (PDFium + pdf.js + veraPDF + own) | B8 | 10k docs: zero crashes, render diffs triaged and baselined |
| **J4** | Fuzzing — libFuzzer on the engine façade | B2,B6 | 72 h clean run; corpus minimised and checked in |
| **J5** | Security: out-of-process workers, sandbox per platform | B2 | Worker crash does not take down the app; seccomp/AppContainer active |
| **J6** | Perf gate tuning on dedicated hardware | C6 | All §14.1 budgets green; gates not flaky |
| **J7** | Crash reporting + opt-in telemetry | A8 | Symbolicated stacks from all 6 platforms |
| **J8** | Auto-update (delta where possible) | I8 | Update path tested from n−1 to n on all desktop targets |

---

## 4. Critical path

```
A1 ─ A2 ─┬─ B1 ─ B2 ─ B3 ─ B4 ─┬─ C4 ─ C6 ─┬─ F1 ─ F2 ─ F3 ─┬─ H1 ─┬─ I1 ─ I8 ─ J*
         │                      │           │                 │      └─ I4
         │              B5 ─ D6 ┘   C2 ─ C3 ┘        F4 ─ F5 ─┤      
         └─ B6 ─ B7 ─ B8 ─ D2 ─ D3 ───────────────── H2–H8 ───┘
A3 ─ E1 ─ E2…E8 ─ E9/E10 ─ G1…G8 ──────────────────────────────┘
```

**Longest chain:** A1 → A2 → B2 → B3 → B4 → C4 → F1 → F3 → H1 → I1 → I8 → J1.

**Parallel from day one:** Block E (UI kit) depends only on A3, so the UI and the engine can be built simultaneously and meet at F1. If you have two engineers, this is the split — one on A/B/C/D, one on A3/E/G.

---

## 5. Where to spend review attention

Most WPs are mechanical. These eight are where silent, expensive mistakes live:

| WP | Risk if wrong |
|---|---|
| **B1** `DocumentSnapshot` | Everything downstream reads corrupt bytes; symptoms appear far away |
| **B6** `EditGuard` | Both engines mutate → screen stops matching saved file |
| **B8** Round-trip | Silent data loss on save; users lose signatures and tag trees |
| **C2** `RasterCache` | Unbounded growth → OOM on mobile after 20 minutes |
| **D3** `EditJournal` | Undo that doesn't fully invert is worse than no undo |
| **F3** Gesture handling | Any re-raster during gesture and the 60 fps target is gone |
| **H2** `/AP` generation | Annotations that render only in Folio; invisible in Acrobat |
| **H4** Redaction | Covered-but-not-removed content is a legal liability, not a bug |

---

## 6. Standing gates

Run on every merge, from the week each becomes applicable:

1. All 6 platforms configure + build
2. `ctest` green
3. **B8 round-trip byte-identical**
4. 52+ golden images, cross-platform ΔE2000 < 1.0
5. Perf p99 within §14.1 budgets
6. No `#include <Q…>` in `folio-core/`
7. No hard-coded colour or duration in `folio-ui/`
8. Corpus: zero new crashes

If a gate becomes flaky, **fix the gate the same week**. A disabled gate is how projects lose their quality floor without noticing.

---

## 7. Week-one concrete start

In order, no branching:

1. **A1** — tree, CMake, presets, vcpkg manifest. Empty build green on 3 desktop platforms.
2. **A2** — vendor PDFium prebuilts, PoDoFo 1.0 via vcpkg. `FPDF_InitLibrary()` + `PdfMemDocument{}` link and run.
3. **A7** — push the 6-platform CI matrix *now*, while the codebase is trivial.
4. **A3** — token generator. `design-tokens.json` → `Tokens.qml` + `folio_tokens.h`.
5. **A4, A6, A8** — utilities, test harness, core-purity guard.
6. **B1** — `DocumentSnapshot` with the rope and fuzz test.
7. **B2 + B3** — load through the rope, render one tile.
8. **F1 (spike)** — throw that tile on screen in a `QQuickItem`.

**End of week one you have a window showing a real PDF page rendered through the full snapshot → PDFium → tile → GPU path.** Everything after that is filling in.
