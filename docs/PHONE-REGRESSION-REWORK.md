# Phone regressions: second corrective pass (2026-10-14)

> **Superseded by `ANDROID-STATUS-BUFFER-ROOT-CAUSE.md`.** The user's
> `uploads/image-1.png` disproved this pass's content-padding fix. They also
> reported that the subsequent `folio-android-safearea-zoom-debug.apk` fixed
> zoom but **still did not move Home/PDF titles below Android's clock**.
> Both earlier APKs have been removed; neither is the current deliverable.

The earlier APK passed compile/lint but **did not fix the user's phone**. It must not be described as device-verified. The screenshot `uploads/photo_6230801239650275798_w.jpg` visibly shows time/status icons drawn over the Folio title row. The affected original PDFs and a connected phone are not available in the sandbox.

## Changes in this pass

1. **Status/title overlap:** instead of only setting decor/window flags, `FolioActivity.installContentInsets()` applies the *remaining* Android status inset as top padding to the actual `android.R.id.content` container holding NativeActivity's SurfaceView. It compares the bar inset with the content view's `getLocationOnScreen()` so a device that already places content beneath the bar receives **zero** extra padding. Both the rendered native surface and its input target shift together. It reapplies on layout/inset changes and keeps the system bar background colour coordinated with Folio's light/dark theme. A status-bar-height resource is a fallback if an OEM consumes both inset callbacks while leaving the bar visible. On a phone, `adb logcat -s folio` now reports the applied top padding when it changes. **Still needs on-device verification.**
2. **Thumbnail panel:** traced a concrete drop: PDFium completion and focus-switch logic discarded thumbnails more than two pages from the main canvas, even if the user had scrolled the Pages panel to those pages. The panel now publishes its visible page range, the PDF worker preserves/prioritises small renders in that range (one after two canvas tiles), and the source accepts/caches their results. It releases distant thumbnails when the panel closes or the visible range changes. A real 150+-page PDF regression checks that page 45's thumbnail resolves while the canvas is focused on page 2.
3. **Touch physics:** a clear horizontal *one-finger* gesture on an enlarged PDF can now pan it; a vertical scroll locks out horizontal drift. Inertia uses bounded velocity and exponential friction instead of the former too-short glide. Page edges get a bounded (~64 logical px) resisted pull and a critically damped spring back; the sidebar list has a similar 60px limit/spring. Resting UI stops animating. These are code-level physics parameters, **not device-measured feel**.
4. **Zoom anchor:** the focal X fraction is constrained to the PDF page when a pinch or pointer falls in its margin; moving off-centre pinch regression checks the same PDF point stays beneath the moving fingers. Viewport X is still clamped at page bounds; moving a zoomed page sideways now uses the deliberate horizontal drag described above.

## Build and verification

- Linux CTest: **18/18 passed**, including off-centre pinch, intentional horizontal pan, vertical lock, overscroll scheduling and distant visible thumbnails.
- Rebuilt using `tools/build-apk-clean.sh`: Android `assembleDebug` and `lintDebug` passed for the arm64-v8a and x86_64 build; temporary SDK/Gradle/CMake trees were removed. Latest artifact: `/home/user/folio-android-corrected-debug.apk`, SHA-256 `836a97f04cea06e2f939e350fb54529f29c1e8c81f1c5617780028d5dff06fed`. APK ZIP integrity was checked by the wrapper. This **does not prove the status inset or gesture physics on the user's phone**.
- The sandbox cannot run a real Android device/emulator. Do not say the overlap, scroll feel, zoom point or sidebar behaviour are definitively fixed until the user tests this APK. If it still overlaps, collect phone model/Android version and `adb logcat -s folio` at launch/resume plus a fresh screenshot.

## Windows build blocker

The user asked for `clang-cl` and a Windows build. A Debian Clang 19 executable was unpacked **under `/var/tmp`**, not into the <125 MB workspace; a `clang-cl` driver symlink ran and reported target `x86_64-pc-windows-msvc`. A compile probe with `#include <windows.h>` failed (`windows.h` not found); this sandbox has no Windows SDK, MSVC/CRT import libraries or Windows runner to link/test a native Folio.exe. No Windows ZIP was built and no old/unverified artifact is being offered. The canonical clean Windows build script/template remains in `tools/build-windows-clean.ps1` and `github-workflows/`. A real Windows runner or Windows machine plus the latest source is required.

## Workspace

Build work and dependency downloads were placed under `/var/tmp` and removed after validation; source, user uploads and the current APK are retained. The persistent `~/.android/debug.keystore` must be retained so subsequent debug APKs can update previous installations without changing signing identity.

## Addendum: status-bar overlap persisted, zoom-out/in shifts PDF left

The newer `uploads/image-1.png` shows Folio's title/home/document-switcher row
intersecting the status icons despite the Java content padding. The prior APK
was **not** a solution on this phone. A new attempt measures the **actual
NativeActivity SurfaceView position** relative to the Android status inset.
When the surface still lies beneath the status bar, Java sends the uncovered
pixel offset to the native shell; when the surface has already moved below the
bar, it sends zero, preventing double-insetting. The native shell shrinks the
Folio layout viewport, clips/translates the render target, paints the bar
background, and subtracts the same offset from native pointer/wheel Y
coordinates. It responds to later surface relayout and inset changes. This is
an independent fallback rather than another content-padding-only variation.
Its device behaviour still needs testing on the affected phone.

A separate X-anchor bug appeared with mixed-width PDFs: after zooming fully
out, the viewport's vertical midpoint can land on a narrow page while a wider
page grows beyond the viewport. The old condition adjusted horizontal scroll
only when the *anchor* page was wide, so the shared document remained pinned
left. The anchor now adjusts X whenever the **document's common content width**
overflows, preserving an off-centre pinch focal point as well. Two regressions
cover the animated zoom-button transition and a moving two-finger zoom from
minimum scale on a mixed-width document.

- New Android debug APK: `/home/user/folio-android-safearea-zoom-debug.apk`.
  SHA-256: `08b830e54c653a0c0bf90b582f7a53590f23160b31e60c5abd466685ddccdb6f`.
  Android `assembleDebug` and `lintDebug` succeeded. ZIP integrity and both
  native ABIs were checked.
- Linux full-suite CTest: **18/18 passed**, including the two new X-anchor
  regressions. A first test draft sampled halfway through the 400 ms
  zoom-out animation and landed in a page gap; the test was corrected to
  sample after settling, then all 18 passed.
- **No phone execution occurred.** A successful build/test does not establish
  that the status area is correct on this OEM device. Verify on the user's
  phone; if overlap remains, collect a screenshot and `adb logcat -s folio`
  showing `content top safe padding`, `native safe top`, `surfaceScreenY` and
  `statusInset`, plus phone model and Android version.
- The earlier Windows limitation still applies: `clang-cl` was installed and
  probed in temp, but no genuine Windows executable was built without Windows
  SDK/MSVC libraries or a Windows runner.
