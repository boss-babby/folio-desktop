# GitHub Actions workflows

This workspace will not let me create a `.github` folder, so the workflow
files live here as plain, visible files. **They do nothing where they are.**
GitHub only runs workflows from `.github/workflows/`.

## Installing them

In your clone, after downloading this folder:

```bash
mkdir -p .github/workflows
cp github-workflows/release-windows.yml .github/workflows/
cp github-workflows/ci.yml             .github/workflows/
git add .github
git commit -m "Add CI and Windows release workflows"
git push
```

On Windows:

```bat
mkdir .github\workflows
copy github-workflows\release-windows.yml .github\workflows\
copy github-workflows\ci.yml              .github\workflows\
git add .github && git commit -m "Add workflows" && git push
```

> `.github` begins with a dot, so Explorer and most file pickers hide it.
> Creating it from a terminal, or from "New Folder" named `.github.`
> (with a trailing dot, which Windows strips), both work.

## Which file does what

| File | Trigger | Produces |
|---|---|---|
| `release-windows.yml` | manual, or a `v*` tag | `Folio-windows-x64.zip` as an artifact, plus a GitHub Release on a tag |
| `android-probe.yml` | manual | a cross-compile check only — **there is no Android app yet**, see `ANDROID-READINESS.md` |
| `ci.yml` | every push / PR | the standing gates: core-only, full, sanitizers, boundaries, and a Windows build+test |

**If you only want an exe, you only need `release-windows.yml`.**

## Getting your build

1. Push, then open the **Actions** tab.
2. If prompted, click *I understand my workflows, go ahead and enable them*.
3. **Release (Windows)** in the left sidebar → **Run workflow** → *Run workflow*.
4. Wait ~10 minutes. Open the finished run, scroll to **Artifacts**, download
   **Folio-windows-x64**.
5. Unzip. Run `Folio.exe`. Keep `pdfium.dll` and `assets\` beside it.

Full detail, including the local MSVC route, is in `BUILD-WINDOWS.md`.
