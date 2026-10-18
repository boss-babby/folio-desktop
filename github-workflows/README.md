# Windows workflow templates (not active in this directory)

As requested, the canonical Windows workflow YAML files live here:

- `release-windows.yml` — **primary** MSVC x64 GUI build + tests + portable ZIP on push/PR to `main`, manual dispatch, or a `v*` tag; a passing tag also publishes the ZIP to a GitHub Release.
- `windows-full-linked.yml` — separate, **manual-only** MSVC x64 workflow for the fully supported PDFium + Skia reader, annotations and PDFium Save As. It invokes the same fail-closed `tools/build-windows-clean.ps1` and uploads a verified portable ZIP plus SHA-256; it does **not** silently fall back to a PoDoFo-only or core-only build. Choose this when you want an on-demand build without duplicate CI on every push.
- `windows-clang.yml` — optional ClangCL x64 ZIP on manual dispatch or a pull request.
- `ci.yml` — optional broader cross-platform CI; its Windows job now invokes the **same** clean builder. If you activate both `ci.yml` and `release-windows.yml`, they will run duplicate Windows builds on a PR/push; activating just `release-windows.yml` is enough for Windows CI.

All Windows build templates call `tools/build-windows-clean.ps1`, which copies
**the current source, including uncommitted files on a local Windows machine**,
to a unique Windows TEMP directory, downloads verified pinned dependencies
there, configures/builds and tests there, verifies at least 19 discovered tests
(including PDFium, Skia and GUI regressions), checks the GUI subsystem, smoke
test and ZIP, then removes that TEMP tree in a `finally` block. The manual-only
workflow keeps a final ZIP and SHA-256 text file in the checkout on its runner;
the automatic release workflow keeps only its final ZIP. It enables the **currently supported**
Windows GUI (PDFium + Skia, PDFium Save As) but deliberately leaves PoDoFo
OFF: incremental in-place save/page-tree editing is not in this scope. No
successful binary is claimed until a Windows runner actually passes the build,
tests and package smoke check.

**GitHub does not discover workflows under `github-workflows/`.** To enable
the separate manual build, copy `windows-full-linked.yml` to
`.github/workflows/windows-full-linked.yml` **in the remote GitHub repository**,
commit/push it with `tools/build-windows-clean.ps1`, `tools/bootstrap-deps.ps1`
and all current source changes, then use Actions → Folio Windows FULL (PDFium + Skia)
→ Run workflow. Alternatively, copy `release-windows.yml` to the matching
`.github/workflows/` path for push/PR CI and version-tag releases. The copy in
`.github/workflows/` is an unavoidable GitHub registration file, not where the
canonical template is authored. If you do not want **any** Windows YAML under
`.github/`, run the PowerShell build script on a Windows PC/runner manually
instead; GitHub Actions **cannot run** a template directly from this folder.

Example setup in a GitHub checkout:

```powershell
New-Item -ItemType Directory -Force .github/workflows | Out-Null
# Pick ONE primary workflow. Manual-only, separate full-linked ZIP:
Copy-Item github-workflows/windows-full-linked.yml .github/workflows/windows-full-linked.yml
# Or, for automatic PR/push CI and tagged GitHub Releases instead:
# Copy-Item github-workflows/release-windows.yml .github/workflows/release-windows.yml
# Optional ClangCL job:
# Copy-Item github-workflows/windows-clang.yml .github/workflows/windows-clang.yml
# Commit and push the copied workflow(s) AND current source changes.
```

A workflow run only builds its **pushed commit**, not edits still present solely
in this conversation's workspace. Download the green manual run's
`Folio-windows-full-x64` artifact (ZIP plus checksum), or the release workflow's
`Folio-windows-x64` artifact; alternatively supply a connected Windows runner
or repository so the assistant can retrieve it. Failure means no verified ZIP is published.
The sandbox here is Linux and has no Windows SDK or GUI runtime; it cannot
manufacture or certify a native Windows executable by itself.

Locally on Windows with VS 2022 C++ workload and CMake >= 3.28:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-clean.ps1
```

The output defaults to `Folio-windows-x64.zip` *next to* the source checkout;
`-OutputPath C:\path\Folio-windows-x64.zip` overrides it. Use `-Compiler
ClangCL` only in an x64 VS developer shell with Ninja and clang-cl available.
