# Clean workspace and repeatable build handoff

## What stays in the shared workspace

- `/home/user/folio-desktop/` — source, tests, documents, instructions and workflow **templates**.
- `/home/user/folio-android-amber-selection-debug.apk` — current Android debug APK with reference-inspired selection and preceding multi-document/text-coordinate changes. The earlier status-bar/zoom repairs were user-confirmed; the new selection is **build-verified, not yet phone-verified**.
- `/home/user/uploads/` — user-provided material.
- Once a real Windows runner completes successfully: `/home/user/Folio-windows-x64.zip` (portable EXE + PDFium DLL + fonts) may be copied here as a **final deliverable only**.

There is **no Windows ZIP yet**. A Windows SDK/runner and a new successful build are required. Never relabel the Linux executable or an unverified/old ZIP as a Windows build.

## Android: future clean builds

Run `bash tools/build-apk-clean.sh` from anywhere. It copies the current source
(including uncommitted changes) into a unique `/var/tmp/folio-android-build.*`
directory, sets Android SDK/JDK/Gradle/native dependency roots there, calls the
existing `tools/build-apk.sh` (assembleDebug + lintDebug), tests ZIP integrity,
and only then replaces `/home/user/folio-android-corrected-debug.apk` by default.
**For the current deliverable name**, supply the explicit absolute output path
`/home/user/folio-android-amber-selection-debug.apk`. The EXIT trap removes the entire
scratch directory even when the build fails.
The existing `~/.android/debug.keystore` (a few KB) must remain stable outside
the build scratch: deleting or regenerating it changes the APK signature and
prevents installing an update over the earlier debug app without uninstalling
and losing its local data. Pass an **absolute** APK path to change the final
location. The existing
`tools/build-apk.sh` is the inner build and should not be called directly if
workspace cleanliness is required. `/var/tmp` must have several GB of free
disk; a new run redownloads toolchains because no dependencies are retained
in the workspace. The first end-to-end attempt exposed an undersized 192 MiB
Gradle Metaspace limit during lint; after raising it to 256 MiB, the clean
wrapper completed **assembleDebug and lintDebug** and removed its scratch
folder. The current Android amber-selection APK's SHA-256 is
`3f0fa2c2b588a8cbb9827bd276d89b1475e088fde8162333d2fc2bb5650447fa`.
This is build validation only, not an on-device retest of the new changes; see
`PDF-TEXT-COORDINATES-AND-SELECTION-2026-10-15.md` for their behaviour and limitations.

## Windows: future clean builds

On Windows, run `powershell -NoProfile -ExecutionPolicy Bypass -File
tools\build-windows-clean.ps1`. The script stages current source (including
uncommitted files), downloads verified dependencies, builds, tests, smoke-checks,
and ZIPs under a unique `%TEMP%\folio-windows-*` directory, cleans it in
`finally`, and only then emits `Folio-windows-x64.zip` beside the source
checkout by default. A failed run leaves a prior ZIP untouched; compare the
fresh build log/hash and do not re-present an older file. See
`BUILD-WINDOWS.md` for Windows prerequisites and limitations. The PowerShell
script has been parser-checked on Linux, but **has not been executed on a
Windows runner yet**.

The canonical workflow templates are in `github-workflows/` as requested.
GitHub Actions cannot run templates at that path: copying the selected YAML
into `.github/workflows/` **in the remote repository** and pushing the latest
source is required. The local checkout deliberately has no active Windows
workflow in `.github/`. Alternatively run the script on a Windows host.
Until a Windows runner/repository is available to this conversation, the
assistant can maintain and prepare the build process but cannot hand over a
validated native Windows ZIP on demand.

## Cleanup/audit

After the process is set up, `android/local.properties` and generated build
folders were removed from the source checkout. Inspect with:

```bash
find /home/user/folio-desktop -maxdepth 5 \
  \( -name build -o -name .cxx -o -name .gradle -o -name CMakeCache.txt -o -name local.properties \) -print
du -sch /home/user/folio-desktop /home/user/*.apk /home/user/uploads
```

Do not delete source files, user uploads, the current APK or a verified final
Windows ZIP during cleanup. The 125 MB budget applies to persisted workspace
content; temporary toolchains/build trees must not be kept there.
