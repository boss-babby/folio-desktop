# Clean, on-demand Windows build

The current workspace is Linux. A Windows `.exe` needs a **real Windows SDK and
MSVC-compatible compiler**; no native Windows binary is claimed to exist in
this workspace yet. `tools/build-windows-clean.ps1` is the repeatable build
entry point for a Windows PC or hosted Windows runner. It builds the current
source, tests it, checks that `Folio.exe` is a GUI (not console) program,
smoke-tests it, and emits one portable ZIP containing `Folio.exe`,
`pdfium.dll`, fonts, sample documents and a README.

## Workspace contract

- The script makes a unique working directory under Windows `%TEMP%` and
  copies the checkout there, **including current uncommitted source**.
  Downloaded Skia/PDFium, CMake files, objects, test logs, staging and the
  intermediate ZIP stay under that directory. It removes the whole working
  directory on both success and failure. It never runs `cmake -B build` in
  the source checkout.
- Only after all tests, checks and the ZIP integrity check pass is a final
  `Folio-windows-x64.zip` copied next to the checkout by default. `-OutputPath`
  selects another final location. On failure, an earlier final ZIP is left
  untouched; **do not mistake that older file for a fresh build**.
- The script requires temporary free disk space for the Windows prebuilts and
  native build. It downloads them again on a new invocation so no build cache
  lives in the workspace. No admin rights are needed.
- `FOLIO_WITH_PODOFO=OFF` is deliberate: PDFium Save As works for the supported
  annotation types, but in-place incremental save/page-tree writing remains
  unavailable. See [PoDoFo scope](#podofo-scope).

## Local Windows machine

Use Windows 10/11 x64 with the Visual Studio 2022 **Desktop development with
C++** workload (Windows SDK), PowerShell 5.1+ and CMake >= 3.28. From a
PowerShell session in the checkout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-clean.ps1
# Optional: -OutputPath 'C:\deliverables\Folio-windows-x64.zip'
```

The script uses the VS 2022 x64 CMake generator by default. For ClangCL,
launch from an **x64 Native Tools** shell with clang-cl and Ninja available:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-clean.ps1 -Compiler ClangCL
```

ClangCL still needs the MSVC libraries and Windows SDK. MinGW cannot link the
pinned MSVC-ABI Skia `.lib` files. When the build succeeds the script prints
the final path and SHA-256. Unzip the package before running `Folio.exe`;
`pdfium.dll` and `assets/fonts` must stay alongside it. The app is unsigned,
so Windows SmartScreen may ask for confirmation.

## Hosted Windows runner / GitHub Actions

The **canonical workflow templates** are `github-workflows/release-windows.yml`
and `github-workflows/windows-clang.yml`, **not** in `.github/`. As required by
GitHub itself, to activate one you must copy its YAML to `.github/workflows/`
**in the remote repository** and push it together with the latest source.
Keeping the template only in `github-workflows/` does not register an Action.
This checkout deliberately has no active Windows YAML in `.github/workflows/`.
See [github-workflows/README.md](github-workflows/README.md) for the copy
commands and constraints.

After activation, Actions → **Windows x64 (build, test, package)** → **Run
workflow** builds, tests and uploads `Folio-windows-x64` (ZIP inside). It also
runs on pushes and pull requests to `main`; a `v*` tag publishes a release
**only after** the build, required test inventory, full CTest suite, GUI
subsystem/smoke test and ZIP check pass. A failed job uploads **no** verified
Windows ZIP. The ClangCL template provides a separate optional compiler gate.
If activating the broader `github-workflows/ci.yml` too, its Windows job uses
the same builder, so PR/push Windows jobs would be duplicated. Hosted runs build the **pushed Git commit**, not unpushed edits
in this workspace. A remote runner and a way to fetch its artifact are
necessary before this assistant can actually hand over a Windows package;
there is no connected runner or Windows SDK here now.

The earlier user-run ClangCL workflow compiled but failed 1/14 tests due to
42 PDFium subpixels differing by 1/255 at 2x; its regression threshold has
since been corrected. **A new green Windows run has not yet happened.**
Build/lint here on Linux is not a substitute for that check.

## PoDoFo scope

Windows packaging uses `-DFOLIO_WITH_PODOFO=OFF`. It can open, render,
search, select text and work with annotations; PDFium Save As can export
highlights, underlines, strikeouts, ink, notes, boxes and ellipses as a
separate PDF. In-place incremental save and page-tree writing remain
PoDoFo-only; free text, stamps and signatures cannot be exported by that
PDFium route and should report failure rather than silently disappearing.
A Windows PoDoFo build via vcpkg is untested and is not advertised as part of
the clean deliverable process.
