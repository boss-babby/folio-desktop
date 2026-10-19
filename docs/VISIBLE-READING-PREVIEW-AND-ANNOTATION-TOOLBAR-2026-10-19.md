# Reading-page priority, zoom sensitivity, and selected-mark toolbar

## What changed

- Each reading-layout paint publishes the whole range of pages that intersect the canvas viewport before the focused page can cancel queued work. PDFium's worker takes their real page metadata first, then one bounded whole-page preview per missing visible page, ahead of remaining sharp tiles. Grid mode keeps its existing separate visible-cell policy. Offscreen ±2 retention remains bounded; this change does not render full neighbouring pages.
- Ctrl+wheel zoom's exponential coefficient changed from 0.12 to 0.16. Pinch scale is raised to the power of 1.3 at the same gesture midpoint. Ordinary wheel page scrolling is unchanged.
- Desktop selection of a Folio-created annotation opens the existing animated popover pattern near/above its visible bounds. The compact four-action card offers colour, applicable width (otherwise disabled), opacity and delete. The first three use the existing option menus and undoable edit path; delete uses the existing grouped deletion/undo/persistence callback. Imported PDF annotations, touch selections, and compact/mobile layouts do not show the new card. The inspector remains available.

## Verification and limits

- Full Linux PDFium+Skia+PoDoFo CTest suite: **19/19 passed**, including the new visible multi-page preview/metadata and annotation-anchor checks. No display-server visual inspection was possible.
- Android arm64-v8a + x86_64 `assembleDebug`, `lintDebug` and APK ZIP integrity passed. The rebuilt APK is `/home/user/Folio-android-reading-preview-debug.apk`, SHA-256 `f4c3467b587d0f150638f69da2f8b45533f88783b0cf9243c5a65ac39861ea25`. No emulator or physical phone was available; this is not device-performance or gesture testing.
- The worker remains serial per document. Prioritizing pending visible metadata and previews cannot interrupt an already in-flight PDFium render; huge or malformed PDF pages may still take noticeable time. Tiles remain bounded to the visible region; existing memory ceilings and cache eviction apply.
