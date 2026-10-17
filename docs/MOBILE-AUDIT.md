# Folio mobile implementation and audit

**Audit date:** 10 October 2026  
**Live shared-UI preview:** https://8080-iolktppd7v1fpntn6m5a7.e2b.app/  
**Android debug APK at the time:** removed during cleanup; the latest previously built APK is `/home/user/folio-android-parity-debug.apk` (signed debug build; arm64-v8a and x86_64; Android 8.0+, API 26). The live page is **not** an Android emulator or an APK running in a browser. It is the same C++/Skia widget layer, rendered by the Linux streaming shell at **390 × 844**. The stream includes sample PDFs on the host; it cannot browse a visitor's local files. Multiple visitors use the same session. Open it through the **Arena live-preview card** in this conversation: the direct e2b URL may return HTTP 403 outside the authenticated preview session (traffic access token required). It is not a public deployment.

## What changed

| Area | Implemented change | Where |
|---|---|---|
| Touch input | Independent capture by pointer ID; a drag that starts on a card/row can transfer to its scrollable ancestor after touch slop; cancelled gestures release contacts. | `folio-ui/src/Widget.cpp`, `include/folio/ui/Widget.h`, `components/Containers.cpp` |
| Scrolling | Touch dragging and bounded kinetic fling on the scroll container; scrolling does not count as opening a card. | `folio-ui/components/Containers.{h,cpp}` |
| Phone file dialog | Responsive, viewport-constrained open/save card with scrollable document rows and visible actions in portrait and landscape. | `folio-ui/components/OpenDialog.{h,cpp}` |
| Compact chrome | Hide meaningless desktop window buttons on narrow screens; expose Open on the welcome screen, Home and controls on the reader; Android Back can dismiss or leave the reader. | `folio-ui/screens/{WelcomeScreen,ReaderScreen,FolioApp}.cpp`, `folio-ui/shell/NativeShell.cpp` |
| Android documents | NativeActivity subclass launches asynchronous Android Storage Access Framework `ACTION_OPEN_DOCUMENT`, accepts `ACTION_VIEW` PDF `content://` intents, copies incoming URI to private cache off the UI thread, then sends the path to the existing PDFium reader on the native UI thread. Picker cancellation and results are queued safely. No broad storage permission. | `android/app/src/main/java/ai/folio/reader/FolioActivity.java`, manifest, `folio-platform/android/AndroidWindow.cpp`, `folio-ui/shell/NativeShell.cpp` |
| Android typing/clipboard | Hidden 1×1 text input connection for soft-keyboard composition/commit, deletion and Done; keyboard focus notifications for fields and text annotations; Android ClipboardManager for copy/paste service. | `FolioActivity.java`, `folio-platform/android/AndroidWindow.cpp`, `folio-ui/src/Widget.cpp`, `folio-ui/components/PageStage.cpp` |
| Android packaging/polish | Exported JNI entry points, icon, no fake recent PDFs on Android, no backups of imported private working copies, manifest lint clean. | `folio-ui/CMakeLists.txt`, `android/app/{build.gradle.kts,src/main}` |
| Regression tests | Touch tap versus drag, per-finger capture and cancellation, portrait and landscape modal action geometry. | `tests/ui/test_mobile_input.cpp` |

**Other current-tree maintenance:** Windows Skia system-library link dependencies in `cmake/FolioDependencies.cmake`, Skia text/font fallback in `folio-render/skia/SkiaTextSystem.cpp`, and PDFium corpus-test type hygiene. These were checked by their applicable host builds/tests, not by a Windows run here.

## Codebase analysis

- **Core and engine:** `folio-core` owns portable document snapshots, tile geometry, cache and scheduling, independent of windowing. `folio-engine/pdfium` provides rendering/text/search for real PDFs and `folio-engine/podofo` handles writable PDF operations where linked. This boundary is sound for a shared desktop/mobile reader; the important asymmetry is that **the Android target does not link PoDoFo**, so its Save, edit/export and some page operations are not implemented there.
- **Rendering:** `folio-render`'s paint interface decouples widgets from Skia. `folio-render/skia` implements CPU and GPU targets; `NativeShell` scales drawing and input consistently by DPR and falls back to CPU painting if the GPU target fails. The streaming shell renders frames offscreen, not via Android EGL; its visual audit does not verify Android surface recovery or GPU behavior.
- **Platform:** `folio-platform` converts Android batched contacts into per-pointer events and handles activity/surface lifecycle. The Java NativeActivity subclass is a narrow host bridge: SAF, IME, clipboard. Java results are mutex-queued and delivered in the native event loop. JNI library loading and exported symbols were checked in the packaged APK. Activity recreation, density changes, clipboard/IME behavior and picker permission expiry require real-device testing.
- **UI and shell:** `folio-ui` is a custom, Skia-painted widget tree with an `EventRouter`, responsive Welcome and Reader screens, a page stage, modal picker and overlays. Touch capture and scroll arbitration are now handled centrally rather than duplicating phone-specific screen logic. The headless `folio-stream` shell exposes this **shared UI** for interaction and screenshots. It does not contain Android services; its in-app file dialog explores files on the sandbox host, whereas Android uses the system picker.
- **State and functionality:** The stream/desktop welcome intentionally retains sample cards and PDFs for review. Android starts with an empty-state prompt rather than inventing a user's recents. Actual opened documents live in reader tabs for this session; durable recents, reading position, annotation journal and import management still need implementation. The Share and Settings controls are prototype affordances, not complete Android integrations.

## Verification performed

1. **Linux Ninja:** `cmake --build build/linux-audit -j 2` succeeded after the fixes. `ctest --test-dir build/linux-audit --output-on-failure -j 2`: **13/13 passed**, including `ui.mobile.input` and PDFium/integration tests, after the final test cleanup.
2. **Live visual test:** Linux stream returned HTTP 200. Interacted at 390 × 844: welcome → Open PDF dialog → real `Annual_Report_2026.pdf` → Next; the control showed **2 / 24**. The open dialog showed both Cancel and Open inside the viewport. Captures: [Welcome](mobile-audit-welcome.jpg), [Open dialog](mobile-audit-open-dialog.jpg), [Reader](mobile-audit-reader.jpg), [Page navigation](mobile-audit-page2.jpg).
3. **Android package:** Gradle `:app:assembleDebug :app:lintDebug` **succeeded** with **no lint issues**; Java and native C++ compiled for arm64-v8a and x86_64. The APK contains `classes.dex`, PDFium and both native ABI libraries, exports `ANativeActivity_onCreate` and Java callbacks, and passes APK Signature Scheme v2 verification. `:app:testDebugUnitTest` previously reported **NO-SOURCE**, not passing behavioral tests.
4. **Not performed:** no Android device or emulator was available. SAF chooser, opening external providers, soft-keyboard composition, clipboard, gestures, actual phone rotation, memory/performance and TalkBack have **not** been validated on Android. Linux stream tests must not be described as Android runtime tests.

## Remaining work, in priority order

1. **P0 — durable mobile editing/export:** link a compatible Android PoDoFo (or another writer), provide `ACTION_CREATE_DOCUMENT`/provider-backed export, and explicitly handle save failures. Currently Android's Save operation reports “Built without PoDoFo”; annotation editing is session-only. Do not use it for valuable unsaved work.
2. **P0 — device acceptance:** install the debug APK on arm64 and x86_64 devices/emulators; test cold launch, external Drive/Files PDF import, cancellation, rotation/background/low-memory lifecycle, multiline/non-Latin IME, Bluetooth keyboards, clipboard, multi-touch/pinch and GPU/software fallback. Capture logcat and instrumentation results.
3. **P1 — persistence and file hygiene:** persist URI permissions or move to managed durable copies, store and restore real recents/reading state, bound/delete staged cached PDFs, report failed imports to the user, and test oversized/encrypted/corrupt PDFs. Current import uses a temporary app-private cache file whose lifetime is not guaranteed.
4. **P1 — accessibility/mobile polish:** custom-painted widgets lack native semantic nodes/TalkBack navigation. Audit minimum touch target sizes, contrast, screen insets/cutouts, landscape on small devices, font scaling and virtual-keyboard occlusion. The visual preview alone cannot certify these.
5. **P2 — complete prototype features and tests:** implement Share and Settings; expand Android instrumentation and UI tests for picker result ordering, gesture transfer, focus/IME and landscape layouts. Remove stale demo-only pathways and review remaining warning-prone native code.

### Reproduce

```sh
# Linux, from repository root; configure dependencies as in tools/dev-setup.sh first
cmake --build build/linux-audit -j 2
ctest --test-dir build/linux-audit --output-on-failure -j 2
./build/linux-audit/folio-ui/folio-stream --port 8080 --size 390x844

# Android, after tools/build-apk.sh has installed SDK/NDK/Gradle dependencies
cd android
gradle --no-daemon --max-workers=1 \
  -Dorg.gradle.jvmargs='-Xmx768m -XX:MaxMetaspaceSize=384m' \
  :app:assembleDebug :app:lintDebug
```

The low Gradle heap and one worker are necessary in this approximately 2 GB sandbox; the first unrestricted Gradle daemon was killed during native compilation. Retrying with constrained resources completed successfully. The live preview is ephemeral and remains available only while the running workspace process exists.
