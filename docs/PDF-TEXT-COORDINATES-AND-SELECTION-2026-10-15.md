# Text positioning and selection pass (2026-10-15)

> The APK described in the initial pass below has been superseded by
> `/home/user/folio-android-amber-selection-debug.apk`. The text positioning
> fix remains in that build, with the reference-inspired selection below.

## Reference-inspired selection revision

The user supplied a sepia-page screenshot and explicitly requested its warm
translucent rectangular selection, **only slightly rounded**, with the
suggested refinements. Folio now paints each selected text line as a small
padded band with a subtle vertical amber gradient and a quiet 0.85dp border;
corner radius is capped at 2.5dp, not pill-shaped. It retains the prior
visible-text coordinate fix and line/column segmentation. Colour is tuned
separately for Light, Sepia and Dark page themes. Two small end dots have
contrasting halos and invisible 44dp-diameter touch targets; dragging either
adjusts the selected range without page panning. Crossing the other endpoint
continues dragging in the opposite direction. A second finger cancels an
unfinished adjustment and enters the existing pinch gesture. The popup closes
while a handle is moving and reopens upon release with a valid selection.
Only visible selection overlay work is drawn; PDF bitmaps are not re-rendered
for these effects.

Regression tests inspect the painted gradient/outline radii/markers per
page theme, test the 44dp end grip, its selection result and popup callbacks,
and verify a second finger cancels before pinch. Complete Linux suite: **18/18
passed**, Android `assembleDebug` and `lintDebug` passed. Final APK SHA-256:
`3f0fa2c2b588a8cbb9827bd276d89b1475e088fde8162333d2fc2bb5650447fa`.
The exact look and finger feel have not been tested on physical hardware here.

## Diagnosis and change

PDFium's `FPDFText_GetCharBox` yields PDF **user-space** coordinates, while
`FPDF_GetPageSizeByIndexF` gives the **displayed** page width/height. The old
code divided glyph X/Y directly by those dimensions and flipped Y. That
assumes a zero-origin CropBox and no intrinsic `/Rotate`; either assumption
can be false on a valid text PDF. It also used provisional 612×792 page
metadata if the asynchronous text job ran ahead of the metadata job.

Text extraction now asks the already-open PDFium page for actual displayed
size and samples PDFium's `FPDF_PageToDevice` affine transform **three times
per page**, not per glyph. Each glyph's four corners are mapped through that
transform into normalized displayed-page coordinates. Glyphs outside the
visible crop are still searchable, but get an empty visual box; non-finite
coordinates are ignored for selection. Because PDFium applies the same page
crop/intrinsic rotation when rendering, the overlay and pixels share the
same coordinate system without additional rotation on the UI thread.

The live text selection is now a translucent accent wash with rounded ends
and a crisp 1-pixel rounded outline, clipped to the page. It stays an overlay
rather than triggering bitmap re-renders during selection or zoom. Adjacent
characters only merge into one highlight when they share a line **and** are
near each other; distant columns, malformed OCR geometry, and non-finite
boxes cannot become a broad band across the page. Search highlights and
committed markup still have their distinct appearance. The existing markup
popup and its color-picker behavior remain unchanged.

## Regression coverage and limits

- `tools/make-text-coordinate-fixture.py` creates a small reproducible
  text PDF with nonzero CropBox origins, intrinsic 0°/90°/180° rotations,
  and glyphs outside the crop; the generated PDF is in `tests/data/`.
- `ui.pdfium.visible-tiles` extracts the visible and hidden glyphs for all
  three pages and checks that the selection box overlaps **dark pixels in
  PDFium's independently rendered page**, rather than merely fitting the
  page dimensions. `ui.mobile.input` checks that two text columns stay two
  selection segments and that malformed coordinates do not paint a band.
- Complete Linux CTest suite: **18/18 passed** after this pass.
- The user's affected PDF was not attached in this round. Therefore this
  fixes a verified general coordinate mismatch but **does not prove the
  particular PDF is fixed**. Some PDFs contain an intrinsically misregistered
  invisible OCR layer, which needs examination against the original page and
  another viewer's selection to distinguish from a reader transform bug.
- Fresh Android `assembleDebug` and `lintDebug` **passed**. APK:
  `/home/user/folio-android-text-selection-debug.apk` (arm64-v8a, x86_64);
  SHA-256 `d8ac000343ea771ae2e85e4dcb37cca8473acca4c720f96ca999865ac8420997`.
- No physical Android device or runnable emulator exists in the sandbox;
  the new appearance and original-file behavior require phone verification.
