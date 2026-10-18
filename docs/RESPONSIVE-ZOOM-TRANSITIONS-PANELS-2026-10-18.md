# Responsive desktop zoom, navigation, sorting and panels — 2026-10-18

## Changes

- Desktop button/preset/Ctrl+wheel zoom uses a **150 ms OutCubic** transition rather than the previous 400 ms ease-in-out tail. Android/touch zoom timing was not changed. Existing zoom anchors, temporary cached-page stretching, PDFium visible tiles and high-resolution idle sharpening remain in place.
- Fit-width/fit-page responds directly to changing viewport geometry while a desktop panel animates; it holds the previous raster scale during width motion and requests crisp tiles after the width settles, instead of restarting a 400 ms zoom on every frame of the panel transition.
- Home ↔ reader cross-fade and basic page entrances take **220 ms**; focus transitions take 220 ms; pane switch takes 160 ms. These values live in `design-tokens.json` and are regenerated into `Tokens.h`. The separate day/night reveal retains its existing 760 ms directional easing. Recent-card entrance staggers at most eight cards, with 20 ms initial delay + 12 ms per card, and the progress fill takes 160 ms. Cards with no reading progress no longer request frames for an invisible progress bar. Sort by size stats each path once rather than repeating filesystem I/O inside the comparator.
- Left/right panel width and content use the same 180 ms transition; the panel clips children (including thumbnail pages) to its moving edge **without clipping the card's own shadow**. A final layout update is forced if a short animation completes before the next frame (e.g. window stalls or resumes). The left Pages list no longer paints over the document before its panel arrives or after its edge retracts.
- **Neighbouring-page prefetch is preserved**: after first focused pixels, the reading view still queues bounded 240×320-or-smaller previews for the previous and next page and a second page in the scroll direction. It still keeps visible tiles first; the many-page grid continues to use its separate cell-sized priority path.

## Validation and limitations

- Linux PDFium+Skia desktop build and full CTest **19/19** passed; targeted tests verify short desktop zoom, fit responding to viewport width, panel clipping at the pixel level, panel shadow and sync, quick screen swap, bounded sort cards, and the earlier adjacent-page preview regression.
- Fresh Android debug build **assembleDebug + lintDebug succeeded**. `/home/user/Folio-android-responsive-debug.apk` was ZIP-tested and copied from that build, SHA-256 `7e77f849264ddb476fd1bc83f0079f57daf083d0ae247dee8f0d2c0953b13a6a`.
- No Windows GUI runner, Android device, or physical pen/mouse was available; actual hardware fluidity cannot be certified by a headless test. The older Windows ZIP and older Android APK do **not** contain these changes. Rebuild the Windows executable from the refreshed source package to get them.
