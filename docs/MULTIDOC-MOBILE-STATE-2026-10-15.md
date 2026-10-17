# Multi-document state and mobile controls (2026-10-15)

> The APK described here was superseded by the text-layer/selection APK; see
> `PDF-TEXT-COORDINATES-AND-SELECTION-2026-10-15.md`. These multi-document
> changes are retained in the newer build.

The user confirmed the preceding Android status-bar buffer fix and zoom fix
work on their phone. This pass retains that implementation; it does not claim
that these new changes have yet been checked on the phone.

## Changes

- Each open document stores a lightweight viewport snapshot before switching:
  visible zoom and zoom mode, X/Y scroll, page, layout, rotation, grid size,
  page theme, annotation selection, Pages/Outline scroll offsets, left-panel
  tab, panel open/closed states and Focus Mode. It keeps the PDFium source and
  worker mapped for a quick return, but suspends the inactive tab's decoded
  page/tile/text visual caches. Returning restores layout before XY scroll,
  with no entry animation. Document-close still frees the worker off the UI
  thread. Search/text selections and transient popups are deliberately **not**
  reused across PDFs, to avoid stale glyph overlays. Undo history is cleared
  on switching to avoid cross-document file edits; it is not claimed as
  preserved.
- Opening a new PDF snaps to its final fit width instead of spending 400 ms
  animating from the previous PDF's zoom. The visible page's low-resolution
  preview and then visible 512-device-pixel tiles take priority. Only after
  a foreground visual result is ready does the worker queue at most two
  240-by-320-pixel neighbouring previews, below visible tiles. No full-page
  offscreen raster preload or idle GPU loop was added.
- The right inspector's Light/Sepia/Dark swatches now update the actual
  PageStage page theme (not just AppState/the chip), and are synchronized with
  the Display menu. A pointer-dispatched inspector click is regression tested.
- Compact phone toolbar retains an Open PDF shortcut and shows Display and
  Focus Mode instead of leaving the middle largely empty. Focus has a floating
  exit button when the chrome hides.
- In Select/Read mode, an existing ink/shape annotation captures one finger
  before page panning. Dragging moves the mark on its original page and
  records one undoable move at release; a second finger cancels the uncommitted
  drag and hands control to pinch. Armed drawing/shape tools still draw with
  one finger. On phones, selecting a mark no longer raises the inspector over
  the active drag; its corner handle remains available.
- Thumbnail/outline scroll bounds are updated at source-switch time, and
  their restored offsets cancel old animated reveal targets.

## Verification

- Linux CTest: **18/18 passed** after the final source changes, including
  mixed-width zoom, real-PDF two-tab snapshot return and per-document page
  theme, a real right-panel pointer click, phone toolbar/focus entry and exit,
  one-finger box and ink moves, pinch interruption and adjacent proxy work.
- Android `assembleDebug` and `lintDebug` **passed**. Both arm64-v8a and x86_64
  are packaged. APK: `/home/user/folio-android-multidoc-debug.apk`.
  SHA-256: `5d7b2d10613cd6b0f0065caa308b241d1a2ede35743eff6a6cb83348006a1b85`.
- No connected Android device or runnable emulator is available in this
  sandbox. Mobile interaction/latency and exact UI placement for this new pass
  require user verification on the phone.
- The previous `folio-android-buffer-debug.apk` was confirmed by the user for
  status-bar layout but has been removed as a superseded workspace artifact.
