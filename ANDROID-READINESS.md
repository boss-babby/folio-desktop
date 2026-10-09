# Is Folio ready to build for Android on GitHub?

**No.** Not close, and not for the reason you might expect.

The short version: **the dependencies are ready, the application is not.**
Nothing is blocked by a third party. What is missing is Folio's own Android
layer, and it is roughly the same amount of work the Windows backend was —
except that, unlike Windows, Android also needs an app wrapper, a different
file-access model, a different asset model, and a different *input* model.

---

## What is genuinely ready

I checked these against the live release feeds rather than assuming.

| | Status |
|---|---|
| **Skia** | skia-pack ships `Skia-m144-e2e6623374-4-android-Release-arm64.zip` and `-x64.zip` — **on the exact tag we already pin.** No version shopping needed. |
| **PDFium** | pdfium-binaries ships `pdfium-android-arm64/x64/arm/x86.tgz` on current `chromium/8086`. |
| **`tools/bootstrap-deps.sh`** | now takes `FOLIO_TARGET=android FOLIO_ABI=arm64` and fetches those. Both URL patterns were resolved against the real asset lists; the Linux defaults are byte-for-byte unchanged. |
| **CMake platform detection** | `FOLIO_PLATFORM=android` and `FOLIO_MOBILE=ON` already exist in `cmake/FolioOptions.cmake`. |
| **GitHub Actions** | `ubuntu-latest` ships an Android SDK + NDK. No self-hosted runner, no Mac, no licence. CI is a perfectly good Android build box. |
| **The portable two-thirds** | `folio-core`, `folio-render`, `folio-engine` and `folio-ui-core` are plain C++ with no OS calls. They *should* cross-compile. Unverified — see the probe below. |

One gap worth knowing: skia-pack publishes **no `armeabi-v7a`** slice.
arm64 and x86_64 only. That is fine — Google Play has required 64-bit since
2019 — but it rules out genuinely old hardware.

---

## What does not exist

### 1. No Android platform backend

`folio-platform/` contains `linux/X11Window.cpp`, `win32/Win32Window.cpp`
and `src/PlatformNone.cpp`. There is **no `android/`**. Worse, every real
branch in `folio-platform/CMakeLists.txt` explicitly excludes it:

```cmake
elseif(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
```

So configuring for Android today silently selects the `none` backend: it
compiles, links, and opens no window. It would look like success and
produce a library that cannot draw.

Needed: `AndroidWindow.cpp` implementing `IWindow`/`IGlContext` over
`ANativeWindow` + EGL, driven by the activity lifecycle.

### 2. No app wrapper

An APK needs an entry point. Either `NativeActivity` with
`android_native_app_glue`, or a Kotlin `Activity` plus a JNI bridge. There
is no `AndroidManifest.xml`, no Gradle project, no Kotlin, no JNI — the
repo has zero Android project files. The Kotlin route is more work up front
but is the only one that makes the next two items tractable.

### 3. File access is a different model entirely

`IFileDialog` is implemented by shelling out to `zenity`/`kdialog`. Android
has no filesystem browsing for user documents — it has the **Storage Access
Framework**: you fire an `ACTION_OPEN_DOCUMENT` intent, get a `content://`
URI back, and read through a `ParcelFileDescriptor`. PDFium is happy to
read from a file descriptor, so this is tractable, but it means
`openFile(std::string path)` is the wrong signature on Android and the
in-app `OpenDialog` is the wrong UI.

**Saving is worse.** The incremental-save path writes back to a path on
disk. Under SAF you get a write descriptor, not a path.

### 4. Assets are not files

`NativeShell::resolveAssetDir()` — which I added last round — walks the
filesystem next to the executable. Inside an APK the fonts are not files;
they are entries in the archive, reachable only through `AAssetManager`.
Skia needs a font manager fed from that instead.

### 5. Touch — and this is the real one

I reported last round that `PointerEvent` has no pointer type, no pressure,
no tilt, and that no backend emits touch at all. On desktop that is a
quality gap. **On Android it is the entire input model.** There is no
mouse. Shipping the current event struct to a phone means no pinch zoom, no
two-finger pan, no fling, no long-press, and a stylus that behaves like a
finger.

The phone layout from Round 17 is real and I am happy with it — but it is
currently driven by a mouse event model, and Android is where that stops
being survivable.

---

## Ordered plan

Each step is independently verifiable, which matters because nobody can
eyeball an APK from a CI log.

| # | Work | Verifiable by |
|---|---|---|
| 1 | **Probe the portable layers** (`github-workflows/android-probe.yml`) | CI goes green, or hands you a short portability list |
| 2 | Touch/pen in `PointerEvent` + XInput2 on Linux | pinch/pan testable on the desktop build first |
| 3 | `AndroidWindow.cpp` — `ANativeWindow` + EGL + lifecycle | a solid-colour frame on an emulator |
| 4 | Kotlin activity + Gradle + JNI bridge | an APK that installs and launches |
| 5 | SAF open/save behind `IFileDialog` | open a PDF from Drive |
| 6 | `AAssetManager` font loading | text renders |
| 7 | Gradle CI job, signed debug APK as an artifact | downloadable APK |

Steps 1 and 2 are worth doing regardless of whether Android ever ships —
the first costs ten minutes, and the second improves the desktop and phone
builds on their own merits.

---

## Start here

`github-workflows/android-probe.yml` answers the first question for the
price of one CI run: **does the portable stack cross-compile for arm64
against the Android prebuilts?** It builds `folio-core`, `folio-render` and
`folio-engine` with the NDK toolchain and `-DFOLIO_BUILD_UI=OFF`, and
deliberately does not attempt the shell — there is nothing there to build.

Install it the same way as the others (see `github-workflows/README.md`),
then run it from the Actions tab. Green means every remaining Android task
is Folio's own code. Red gives you the portability list up front, which is
the cheapest moment to have it.

---

## Honest comparison with Windows

| | Windows | Android |
|---|---|---|
| Prebuilt deps | ready | ready |
| Platform backend | **written** (808 lines, never compiled) | **does not exist** |
| App wrapper | none needed — it is an exe | Gradle + activity + JNI |
| File dialogs | Win32 common dialogs, conventional | SAF; changes the API shape |
| Assets | files beside the exe | `AAssetManager` |
| Input | mouse, already supported | **touch, not supported at all** |
| Realistic distance | one debugging session | several focused sessions |

Windows is "compile it and fix what breaks". Android is a port.
