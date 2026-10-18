# Grid, wheel, inspector, desktop loupe and Windows icon — 2026-10-18

## Source changes

- The grid publishes **every currently visible page** and one adjacent row in each direction to PDFium. Its worker prioritizes the visible interval before background renders, protects buffered requests from the reader's ±2-focus cancellation, and evicts stale offscreen work when the interval changes. Entering grid drops high-resolution reading tiles immediately.
- Grid paints complete, tiny PDFium bitmaps at the **actual displayed per-page size**, including mixed landscape/portrait and rotated page geometry. It no longer uses the reading view's viewport-sized tile path or queues reading-view text extraction/previews in grid. A 36-page mixed-size fixture checks that 18 simultaneous cells farther than ±2 from focus finish with bounded small rasters.
- Ordinary vertical wheel scrolling uses accumulated 112 logical pixels/detent and a short linear 105 ms glide (65 ms for fractional input) instead of restarting a 190 ms slow-start animation per notch. Ctrl+wheel zoom, horizontal/Shift wheel, touch pan and pinch retain their own paths.
- Inspector header wraps rotation controls onto a second row at narrow widths; content and the authoring/delete row start below all controls. Removed the extra parent DISPLAY heading that covered PAGE THEME.
- Desktop mouse selection handles, text-selection drag and highlighter drafts display the existing cached-pixel magnifier **only when explicitly enabled**. Mouse release/cancel hides it. Desktop default remains off; Android default remains on.
- `assets/windows/Folio.ico` contains 16–256 px icon images reproducing the Android `ic_folio.xml` indigo and white mark. A Windows-only `.rc` resource embeds it into the GUI executable and `WNDCLASSEXW` sets large/small window icons.

## Verification and limitations

Linux PDFium + Skia: full CTest **19/19** passed, including grid viewport/worker/render-size tests, wheel accumulation, non-overlapping desktop/phone headers, desktop mouse selection and highlighter magnifier, and existing regression tests. Also built the no-Skia Linux target; 14/15 passed, with the existing Bengali export test requiring Skia. `git diff --check` passed. Windows Explorer/taskbar rendering and actual mouse-wheel hardware feel have **not** been verified on a Windows host. Neither the previous Windows ZIP nor the previous Android APK contains these changes; rebuild them from the updated source before testing those platforms.
