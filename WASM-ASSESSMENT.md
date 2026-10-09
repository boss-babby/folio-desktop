# WebAssembly: what to think about it

**Round 22. Written after Android was made to build, and after checking what
actually ships rather than what the original plan assumed.**

The architecture plan's §13 called WASM "the hard target" and budgeted for a
fight. Two things have changed since it was written, and they move the
answer a long way.

---

## 1. The short version

**Build it, but third, and build it as a reader first.**

WASM is no longer a research project for Folio. The two dependencies that
were supposed to be the hard part — Skia and PDFium — both ship prebuilt
Emscripten artefacts at the exact versions this repository already pins.
The real blocker has moved somewhere less glamorous: **PoDoFo**, and
therefore **saving**.

What makes this different from the §13 assessment:

| §13 assumed | Reality, checked 2026-10 |
|---|---|
| Qt Quick runtime on WASM, **GPLv3 or commercial only** | Irrelevant — we committed to Skia. BSD, no licence problem. |
| 8–15 MB of toolkit runtime | No toolkit. Skia + our own widgets. |
| Skia for WASM = build it yourself | `skia-pack` ships `Skia-m144-...-wasm-Release-wasm.zip`, **23.5 MB** of static archives, same `m144-e2e6623374-4` tag we already use. |
| PDFium for WASM = build it yourself | `pdfium-binaries` ships `pdfium-wasm.tgz`, **2.5 MB**, same release channel (`chromium/8086`) we already use. |

Both are *the same artefacts we already consume on five other platforms*,
from the same two upstreams, at the same pinned versions. That is the
single most important fact in this document: the WASM target does not
introduce a new supply chain, a new Skia build, or a new PDFium
configuration to keep in sync.

---

## 2. What actually blocks it

### 2.1 PoDoFo — the real blocker

PDFium renders; PoDoFo writes. Every save in Folio is a PoDoFo
`SaveUpdate` incremental write, and **there is no prebuilt PoDoFo for
Emscripten**. It would have to be cross-compiled, along with its
dependency chain:

| PoDoFo needs | Emscripten status |
|---|---|
| zlib, libpng, libjpeg-turbo, freetype | `emscripten-ports` — genuinely easy |
| libxml2 | buildable; some configure work |
| OpenSSL | heavy, and only needed for signing |

This is a week or two of build engineering, not a design problem. But
until it is done, **a WASM Folio can open, render, search, select,
annotate in memory and print — and cannot save a file**. That is a
coherent product (it is roughly what most web PDF viewers are), but it has
to be a deliberate decision rather than a surprise.

**Recommendation:** ship web as **reader + annotator**, with
`FOLIO_WASM_MINIMAL` dropping signing and at-rest encryption, and annotate
exports going through "download a copy" rather than in-place save. Then
add PoDoFo when the demand justifies the build work.

### 2.2 Threading and `SharedArrayBuffer`

Unchanged from §13 and still the thing most likely to bite at deployment
rather than development. Threads need cross-origin isolation:

```
Cross-Origin-Opener-Policy:   same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Every third-party resource on the page then needs
`Cross-Origin-Resource-Policy: cross-origin`. **Agree this with whoever
owns hosting before any code is written**, because it can break analytics,
ad tags and embedded fonts, and discovering that late turns a rendering
project into a political one.

The fallback already exists in the architecture: the `IFSDK_PAUSE`
cooperative scheduler from §7.2 yields every ~4 ms, which is exactly the
shape Asyncify needs. Budget ~40 % of threaded throughput.

### 2.3 Accessibility — the honest gap

AccessKit, which the plan selected for accessibility, **has no web
adapter** and no iOS adapter. On the web this matters more than elsewhere,
because the baseline expectation is a DOM that a screen reader can walk,
and a `<canvas>` presents nothing. The mitigation is a parallel offscreen
DOM mirroring the text layer and focusable controls — real work, and it
should be scoped honestly rather than discovered during an accessibility
audit.

This was the original argument against Skia-primary. It has not gone away;
the user's direction was to build on Skia regardless, so it is logged here
as a known cost rather than reopened.

---

## 3. What is already done for it

More than is obvious, because the stream shell forced the same problems to
be solved early:

- **The browser input contract is written and verified.** The stream
  shell's JS already sends `ptype / pointerId / pressure` with
  `touchAction:'none'`, `setPointerCapture` and `getCoalescedEvents()`, and
  the C++ side already parses it into the same `PointerType` /
  `pointerId` / `pressure` fields the X11 and Android backends now fill.
  A WASM backend emits the same events from
  `emscripten_set_pointerdown_callback` instead of a socket.
- **`folio-platform` is the only thing that needs writing.** `IWindow`,
  `IGlContext`, `IClipboard`, `IFileDialog` is a small surface, and the
  Android backend just demonstrated the abstraction survives a platform
  that has no window, no cursor, no title bar and no file paths — which is
  an excellent rehearsal for the browser, which has none of those either.
- **GL is already the renderer.** Ganesh on WebGL2 is the same code path as
  Ganesh on GLES3, which is what Android now runs.
- **`folio-core` is toolkit-free and CI-enforced**, so the core compiles
  for WASM independently of the UI.

### 3.1 The thing to be careful about

`folio-platform/CMakeLists.txt` currently reads:

```cmake
elseif(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
```

so an Emscripten configure selects the **headless `none` backend** and
produces a binary that links, runs, and opens nothing. This is the exact
trap Android was in until this round, and it will look like success in CI.
Whoever starts the WASM backend should make that branch a hard error until
`folio-platform/wasm/` exists.

---

## 4. Size budget

Prebuilt archive sizes are not shipped sizes — Emscripten's `--gc-sections`
equivalent strips hard, and Folio uses a fraction of Skia. Realistic
estimate for a gzipped `-Oz` build:

| Component | Gzipped, shipped |
|---|---|
| Skia (our subset: Ganesh GL, paragraph, no codecs we don't use) | 1.5–2.5 MB |
| PDFium, XFA and V8 off | 2.5–3.5 MB |
| Folio app code | 1–2 MB |
| Fonts (DM Sans variable, subset) | 0.1–0.3 MB |
| **Total first load** | **~5–8 MB** |

Mitigations that are worth the effort, in order: `instantiateStreaming`,
splitting PDFium into a lazily-fetched module so first paint does not wait
for it, and `-Oz` over `-O3` (Folio is not compute-bound in the browser;
it is bandwidth-bound on first load).

---

## 5. Where it belongs in the order

1. **Android** — in progress, now compiling and linking for real.
2. **Windows** — 808 lines written, never compiled. Highest value per hour
   of any remaining work, because the code already exists.
3. **WASM** — after those. It is the best *demo* surface and the cheapest
   distribution, and it is now genuinely reachable.
4. **iOS / macOS** — needs hardware and a developer account; Cocoa is
   unwritten.

**One caveat on ordering:** if the goal is to show Folio to people rather
than ship it to them, the stream shell already does that today, over HTTP,
with no build. WASM's advantage over it is offline use, zero server cost
and real GPU access on the client — all of which matter for a product and
none of which matter for a demo.

---

## 6. Recommendation

Do it, with these conditions written down first:

1. **Reader + annotator, not editor.** No save until PoDoFo cross-compiles.
   Say so in the UI, not just the docs.
2. **Settle COOP/COEP with hosting before writing code.** If isolation is
   refused, accept Asyncify and ~40 % throughput, and set performance
   expectations accordingly — not 60 FPS on a 300-page document.
3. **Budget the accessibility mirror as a feature**, not a polish task.
4. **Make the Emscripten branch of `folio-platform` fail loudly** until a
   real backend exists, so nobody mistakes the `none` backend for a
   working web build.
