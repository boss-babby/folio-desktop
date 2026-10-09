# Building Folio for Windows

Three routes, in order of how little you have to install.

| | You need | Time | Gives you |
|---|---|---|---|
| **A. GitHub Actions** | a GitHub account | ~10 min, unattended | `Folio.exe` as a downloadable zip |
| **B. Local MSVC** | Visual Studio 2022 + CMake | ~15 min first time | a build tree you can debug |
| **C. Tagged release** | a `git tag` | ~10 min | a public GitHub Release page |

Route A is the recommended one. Nothing in this project needs a Windows
machine *you* own — the `windows-2022` runner is a perfectly good build box,
and it is the only Windows machine this code has ever been compiled on.

---

## A. Build on GitHub (no Windows machine)

1. Push this repository to GitHub.
2. **Actions** tab → enable workflows if prompted.
3. Left sidebar → **Release (Windows)** → **Run workflow** → *Run workflow*.
4. When it goes green, open the run and download **Folio-windows-x64** from
   the *Artifacts* box at the bottom.
5. Unzip, run `Folio.exe`.

Leave the *Build with PoDoFo* box unticked the first time. See
[PoDoFo](#podofo-and-what-you-lose-without-it) below for what that costs you.

The ordinary **CI** workflow also builds and tests Windows on every push to
`main`, and uploads the same artifact. Use *Release (Windows)* when you
specifically want a package; use CI to know whether `main` compiles.

### If the Actions run fails

| Symptom | Cause | Fix |
|---|---|---|
| `bootstrap-deps.ps1` 404s | skia-pack retagged the release | Bump `$SkiaTag` in `tools/bootstrap-deps.ps1` |
| `platform backend is not win32` | CMake could not find the Windows SDK and silently chose the `none` backend | Check the *Configure* step log; the guard exists to stop a headless binary being shipped as a desktop app |
| Link errors naming `skia.lib` | 32-bit/64-bit or Debug/Release mismatch | The workflow pins `-A x64` and Release; do not change one without the other |

---

## B. Build locally with MSVC

### Prerequisites

* **Visual Studio 2022**, with the *Desktop development with C++* workload.
  The free Community edition is enough. The workload brings MSVC, the
  Windows 10/11 SDK and CMake.
* **PowerShell 5.1** (ships with Windows) or newer.
* No administrator rights are needed. Nothing is installed system-wide.

### Steps

Open **x64 Native Tools Command Prompt for VS 2022** (not a plain `cmd` — it
sets the compiler environment), then:

```bat
git clone <your-fork-url> folio
cd folio

:: Fetches Skia + PDFium into %LOCALAPPDATA%\folio-deps. Idempotent:
:: safe to re-run, re-uses what it already downloaded. ~250 MB, a few
:: minutes on a normal connection.
powershell -ExecutionPolicy Bypass -File tools\bootstrap-deps.ps1

cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DFOLIO_BUILD_UI=ON ^
      -DFOLIO_WITH_PODOFO=OFF ^
      -DFOLIO_DEPS_DIR="%LOCALAPPDATA%\folio-deps"

cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The executable lands at `build\folio-ui\Release\folio-desktop.exe`.

### Running it from the build tree

```bat
copy "%LOCALAPPDATA%\folio-deps\pdfium\bin\pdfium.dll" build\folio-ui\Release\
build\folio-ui\Release\folio-desktop.exe documents\Annual_Report_2026.pdf
```

Fonts resolve automatically: the shell looks for `assets\fonts` beside the
executable, then one directory up, then at the path baked in at configure
time — which is how a build tree works without copying anything.

### To package it the way CI does

```powershell
mkdir dist
copy build\folio-ui\Release\folio-desktop.exe dist\Folio.exe
copy "$env:LOCALAPPDATA\folio-deps\pdfium\bin\pdfium.dll" dist\
xcopy /E /I assets    dist\assets
xcopy /E /I documents dist\documents
```

All four must travel together.

---

## C. Cut a release

```bash
git tag v0.1.0
git push origin v0.1.0
```

`.github/workflows/release-windows.yml` builds, tests, packages, verifies the
package contains the exe + DLL + fonts, and attaches
`Folio-windows-x64.zip` to a GitHub Release with generated notes.

---

## What actually gets built on Windows

| Target | Windows | Why |
|---|---|---|
| `folio-core`, `folio-render`, `folio-engine` | yes | portable C++ |
| `folio-platform` (`win32` backend) | yes | `Win32Window.cpp` |
| `folio-desktop` → `Folio.exe` | **yes — this is the app** | |
| `folio-stream` | **no** | the HTTP shell is built on BSD sockets; it is a development transport, not a product |
| tests | yes | `ctest -C Release` |

`folio-stream` is the thing you have been clicking in the live preview. It is
deliberately Unix-only; on Windows the native shell is the real app, and
there is nothing the stream shell does that `Folio.exe` does not do better.

---

## PoDoFo, and what you lose without it

CI builds with `-DFOLIO_WITH_PODOFO=OFF`. Without PoDoFo:

* **You keep:** opening, rendering, zoom, search, text selection, every
  annotation tool, undo/redo, page operations in-session.
* **You lose:** writing annotations back into the PDF file. Annotations
  exist for the session and are not saved to disk.

PoDoFo has no official Windows prebuilt that matches this project's layout.
To enable it you would use vcpkg (`vcpkg install podofo:x64-windows`) and
pass `-DFOLIO_WITH_PODOFO=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.
That path is **untested** — flagged rather than claimed.

---

## Honest status of the Windows port

Read this before judging the first build.

* `folio-platform/win32/Win32Window.cpp` is **808 lines that have never been
  compiled by a Windows SDK.** It was written against the same `IWindow`
  interface the working X11 backend implements, but first contact with
  `cl.exe` will almost certainly produce errors. Expect to iterate.
* The CI job has a guard that fails the build if CMake quietly selects the
  `none` backend, so you cannot accidentally ship a window-less binary.
* **There is no single-file exe.** `pdfium-binaries` ships PDFium as a DLL,
  not a static library, so `pdfium.dll` must sit beside `Folio.exe`. A
  genuinely self-contained binary means building PDFium from source with
  `is_component_build=false` — hours of Chromium toolchain, and not worth it
  until the app is otherwise finished.
* **The binary is unsigned**, so SmartScreen will warn on first run. Signing
  needs a purchased certificate.
* MinGW is not an option: skia-pack's `.lib` files are MSVC-ABI, and
  MinGW-w64 uses the Itanium C++ ABI. They cannot be linked together.

## Licence exposure, per dependency

| Dependency | Licence | Obligation if you ship |
|---|---|---|
| Skia | BSD-3-Clause | attribution |
| PDFium | BSD-3-Clause | attribution |
| FreeType (in Skia) | FTL or GPLv2 | attribution under FTL |
| HarfBuzz, ICU (in Skia) | MIT / Unicode | attribution |
| PoDoFo *(optional)* | LGPL-2.0+ **or** MPL-2.0 | if statically linked under LGPL, you must allow relinking; dynamic linking avoids this |

Nothing here is copyleft-viral as configured, **provided PoDoFo is either
left off or dynamically linked.** This is a summary written by an engineer,
not legal advice.
