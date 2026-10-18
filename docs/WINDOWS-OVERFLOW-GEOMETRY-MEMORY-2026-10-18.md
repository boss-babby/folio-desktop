# Windows screenshot follow-up — 18 October 2026

The user's Windows package opened PDFs without the earlier first-click crash. The three screenshots exposed separate selection-overlay, title-bar overflow, and mixed-page-geometry problems. Source changes in this package are **not** a replacement Windows executable; rebuild with the full PDFium + Skia Windows workflow and verify on Windows.

## Changes

1. **Selection/popup artefacts:** removed the per-glyph-line selection outline that produced fine horizontal and vertical box edges behind the floating markup/colour picker. The translucent amber, slightly rounded, glyph-aligned selection *fill* remains. A PDF metadata completion now forces a full retained-surface repaint rather than a popup-sized partial repaint of a changed page layout.
2. **Many PDFs:** the desktop title-bar tab strip clips at its reserved boundary and pans horizontally with the mouse wheel (or horizontal wheel). Selecting a tab brings it into view. The small **All open PDFs** button is fixed immediately before the OS caption controls; its document list scrolls inside a viewport-height-bounded popup. The mobile document-name switcher uses the same list.
3. **Small cover then differently sized body:** later pages use the nearest *resolved* page size as a provisional layout, not always the first page's dimensions. The PDFium worker resolves the focused page and a short neighbour window off-thread, and never composites a page into a provisional rectangle while its real aspect ratio is pending. When sizes arrive, the stage preserves the visible-page scroll anchor and repaints the full surface. Worker focus updates during inertial scrolling before submitting the next page's visible tiles. Arbitrary huge PDFs are not scanned up front.
4. **RAM:** the active document retains visible tiles and small near-page proxies. On tab switch, the outgoing PDF drops full-size tiles and text, retaining at most one 512×768 BGRA page preview; at most four inactive tabs are warm (older ones go cold). On minimize, all background tabs become cold; the active PDF retains at most one 512×768 preview plus three 240×320 neighbours, while the framebuffer, GPU resource cache, Skia copies, and unused PDFium rasters are released. Desktop Skia raster cache budget is now 32 MiB (was 64), PDF image-copy cache 40 MiB (was 64), GPU resource-cache target 40 MiB (was 64). These are soft budgets: a currently visible large viewport can temporarily exceed them rather than blink. Idle cleanup remains one-shot and zero-CPU when settled.

## Validation

- Linux full PDFium + Skia build and test suite: **19/19 CTests passed**; includes Bengali export, visible tiles, selection, partial redraw, navigation, and PDFium Save As.
- New checks cover tab clipping and horizontal wheel hit targets, fixed dropdown geometry and long-list scrolling, page-geometry scroll anchoring, absence of scratchy selection outlines, and inactive/minimized cache bounds.
- Also tested Linux PDFium headless without Skia: 14/14 applicable CTests passed. The Unicode export case requires the Skia font backend, so that case was run in the full 19-test lane instead.
- No Windows GUI or GPU execution was possible here. The screenshot PDFs were not supplied, so actual Windows visuals and Task Manager working set require the user's new build and measurements.

## Important memory trade-off

Inactive tabs still keep their PDFium **document handle and OS-backed file mapping** ready for fast tab switches and safe source access; they no longer retain full decoded page bitmaps. An OS file mapping does not copy the whole PDF into private RAM, but PDFium metadata, the worker thread, mapped-file working set, and allocator state still consume memory. Therefore this does **not** promise constant total process RAM for arbitrarily many open PDFs. Automatically closing every inactive document would make a tab switch reparse the PDF and could fail if a linked source disappears; that requires a separate async reopen and source-access policy before it is safe to ship. Track private bytes and working set separately on Windows when comparing GPU/software modes.
