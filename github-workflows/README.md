# Windows workflow templates (not active in this directory)

As requested, the canonical Windows workflow YAML files live here:

- `release-windows.yml` — **primary** MSVC x64 GUI build + tests + portable ZIP on push/PR to `main`, manual dispatch, or a `v*` tag; a passing tag also publishes the ZIP to a GitHub Release.
- `windows-clang.yml` — optional ClangCL x64 ZIP on manual dispatch or a pull request.
- `ci.yml` — optional broader cross-platform CI; its Windows job now invokes the **same** clean builder. If you activate both `ci.yml` and `release-windows.yml`, they will run duplicate Windows builds on a PR/push; activating just `release-windows.yml` is enough for Windows CI.

Both call `tools/build-windows-clean.ps1`, which copies **the current source,
including uncommitted files on a local Windows machine**, to a unique Windows
TEMP directory, downloads verified pinned dependencies there, configures/builds
and tests there, verifies at least 19 discovered tests (including PDFium,
Skia and GUI regressions), checks the GUI subsystem, smoke test and ZIP, then
removes that TEMP tree in a `finally` block. The only generated file kept in
the checkout by CI is the final ZIP. It enables the **currently supported**
Windows GUI (PDFium + Skia, PDFium Save As) but deliberately leaves PoDoFo
OFF: incremental in-place save/page-tree editing is not in this scope. No
successful binary is claimed until a Windows runner actually passes the build,
tests and package smoke check.

**GitHub does not discover workflows under `github-workflows/`.** To enable the
MSVC template, copy it to `.github/workflows/release-windows.yml` **in the
remote GitHub repository**, commit/push it and then use Actions → Windows x64 (build, test, package) →
Run workflow (or let a push/PR to `main` trigger it). The copy in
`.github/workflows/` is an unavoidable GitHub registration file, not where the
canonical template is authored. If you do not want **any** Windows YAML under
`.github/`, run the PowerShell build script on a Windows PC/runner manually
instead; GitHub Actions **cannot run** a template directly from this folder.

Example setup in a GitHub checkout:

```powershell
New-Item -ItemType Directory -Force .github/workflows | Out-Null
Copy-Item github-workflows/release-windows.yml .github/workflows/release-windows.yml
# Optional ClangCL job:
# Copy-Item github-workflows/windows-clang.yml .github/workflows/windows-clang.yml
# Commit and push the copied workflow(s) AND current source changes.
```

A workflow run only builds its **pushed commit**, not edits still present solely
in this conversation's workspace. Download the green run's
`Folio-windows-x64` artifact, or supply a connected Windows runner/repository
so the assistant can retrieve it. Failure means no verified ZIP is published.
The sandbox here is Linux and has no Windows SDK or GUI runtime; it cannot
manufacture or certify a native Windows executable by itself.

Locally on Windows with VS 2022 C++ workload and CMake >= 3.28:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-clean.ps1
```

The output defaults to `Folio-windows-x64.zip` *next to* the source checkout;
`-OutputPath C:\path\Folio-windows-x64.zip` overrides it. Use `-Compiler
ClangCL` only in an x64 VS developer shell with Ninja and clang-cl available.
