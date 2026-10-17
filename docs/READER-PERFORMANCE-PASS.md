# Reader loading and library covers — 2026-10-15

## What changed

- **Foreground PDF tiles:** the PDFium queue now retains still-visible 512-device-pixel tile jobs across redraws instead of clearing and reinserting them on each frame. A tile already in flight or awaiting UI installation is not queued again. Only tiles outside the *completed* viewport are cancelled. The existing PDF tile grid orders visible jobs centre-first.
- **Nearby page preview:** after the focused page has produced pixels, queue low-resolution neighbours in scroll order: one page on either side and one more in the current direction. Each neighbour is capped at 240 × 320 pixels, using the page's **actual** PDFium geometry on the worker thread, rather than a provisional size inherited from page one. One neighbour may interleave after four foreground tiles so a long viewport does not starve the next page. No adjacent high-DPI tile grid is pre-rendered.
- **Idle/low-memory:** a one-shot idle prune retains only current visible sharp tiles and up to three small nearby proxies. Closing/suspending a tab drops its visual caches; the minimise path also releases in-memory library covers. No recurring polling or rendering timer was added. The existing Android source, Skia image-copy, and Ganesh resource budgets remain in place.
- **Library covers:** each visible recent PDF card gets a real PDFium first-page image (up to 256 × 384 BGRA pixels); the loader processes one file at a time off the UI thread. It requests only cards in or immediately near the viewport. The card preserves page aspect ratio, fills its thumbnail area and clips excess from the **top**, horizontally centring wide pages. Before loading, the existing neutral placeholder remains. An immutable, at-most-8-MiB private disk cache lives beside the recent-document state; keys include the real path, file size and modification time. Stale versions are pruned without a periodic task. The three-second idle pass releases offscreen cover RAM; the minimise path releases all cover RAM. Neither operation deletes the disk cache.

## Verification

- Linux native targets built; `ctest` **19/19 passed**. New checks cover real first-page BGRA, top-aligned crop, persistent reopen, modified-file invalidation, idle/minimise release, forward low-resolution prefetch and duplicate tile coalescing.
- Focused visible-tile test **30/30** and recent-cover test **15/15** across repeated runs with a 512-MiB process virtual-memory cap. These are synthetic tests and fixtures, **not Android phone FPS or RAM measurements**.
- Fresh Android debug APK (`arm64-v8a`, `x86_64`) built successfully and ZIP-verified: `../../folio-android-performance-debug.apk`, SHA-256 `4d5d9742dc8d67f25e12613f9c2aa722a6a9d51e4739840ab55d4e70eb29ab82`.

## Limits and next measurements

The same PDFium engine does not imply identical performance to Google Drive or another reader: input PDFs, hardware, display pipeline, cache budgets and platform integrations vary. Their private scheduling/cache implementations have not been established here. The new build could not be run on an Android phone or the user's affected PDF in this sandbox, so cold-open latency, frame pacing, GPU uploads, power and device RAM still require on-device comparison with those readers using the **same PDFs**. Record time to first page preview, time to fully sharp visible viewport, scroll-frame percentiles, RSS and battery/CPU while idle on the target device before claiming parity.
