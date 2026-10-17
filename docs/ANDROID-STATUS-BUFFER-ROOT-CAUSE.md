# Android status-buffer repair (2026-10-15)

> The user subsequently confirmed this status-bar repair and the zoom fix on
> their phone. The APK described below has been superseded by
> `MULTIDOC-MOBILE-STATE-2026-10-15.md`; the status-buffer code remains in the
> new build.

## User verification status

The user's latest report says the **zoom fix works** but the previously built
`folio-android-safearea-zoom-debug.apk` still paints Home, document switching,
and the PDF title under the system clock/status icons. That APK is rejected and
has been removed from the workspace. The new APK has **not** been run on that
phone; only the user can confirm the visible repair.

## Why the previous safe-area fallback never ran

The preceding `FolioActivity` code searched under `android.R.id.content` for a
`SurfaceView` and only sent `nativeOnSafeArea()` if it found one. Android's
actual `NativeActivity` implementation instead calls `Window.takeSurface(this)`
and adds a `NativeContentView extends View`: there is **no SurfaceView** for this
code to find. Padding the content view cannot relocate the window-owned EGL
surface. See [AOSP NativeActivity.java](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/app/NativeActivity.java).

## New top buffer and alignment

`FolioActivity.installStatusBuffer()` inserts a theme-coloured top buffer view
into the Android content container. It reads the current status-bar inset from
the decor/root and child callbacks; where an OEM consumes both, it uses the
framework's `status_bar_height` dimension, not a hard-coded dp value. It
measures the **window decor screen origin** for the EGL top inset and the
content container screen origin for the Java buffer height. An already fitted
window yields zero rather than a double offset. A global-layout callback
updates the positions for rotation, relayout and system inset changes, but
sends no repeated events when dimensions are unchanged. The system status bar
and its clock/icons are deliberately kept visible and coloured to match the
app's light/dark palette.

The native shell receives the real window-surface top inset via JNI, shrinks
the Folio viewport, paints the buffer strip, clips/translates the app scene,
and subtracts that same offset from pointer and wheel coordinates. This
preserves alignment between the document, header, popups and their touch
areas. No PDF/zoom logic changed in this pass.

## Verification and artifact

- JVM-safe geometry regression: `tests/android/SystemBarGeometryTest.java`,
  **5/5 checks passed** for under-bar, already-fitted, partly-fitted, hidden
  and negative-origin geometry.
- `bash tools/build-apk-clean.sh /home/user/folio-android-buffer-debug.apk`:
  **Android assembleDebug + lintDebug passed**, arm64-v8a and x86_64 native
  libraries present, ZIP integrity checked and JNI safe-area entrypoint
  verified. Temporary toolchains/build trees were removed.
- SHA-256: `fc020432f4ad9c35034953df713653137428fe0e99ed69d9072c8c4aeabebf2c`.
- **No phone/emulator was available.** The result is an implementation and
  build verification, not proof of on-device appearance. If the clock still
  overlaps, obtain a screenshot and `adb logcat -s folio` after launch. The
  relevant lines now report `system buffer`, `native top`, `statusBottom`,
  `windowScreenY`, `contentScreenY` and the JNI event. These numbers distinguish
  an inset calculation failure from a window/render-path failure.
