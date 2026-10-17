# Android PDF export + Windows ClangCL check

## What this pass implements

The Android **Save** action now stages a new, annotated PDF on Folio’s single PDFium worker thread. The imported original is never overwritten. PDFium creates the annotation dictionaries **and visible appearance streams**, writes a private copy, and reopens it for basic validation. Only then does Android show the system **Create document** picker. The selected provider receives the bytes on an I/O thread, not the Android UI thread. Errors and cancellation are shown in the reader; neither clears the in-memory annotations. Repeated exports start from the unmodified imported original plus the current overlay rather than adding duplicate marks. The provider copy uses a truncating file descriptor and syncs before reporting success; a failed new document is deleted where possible.

Supported, tested for visible round-trip: **highlight, underline, strikeout, ink, note, rectangle, ellipse**. Android explicitly refuses **free text, stamps and signatures** for now rather than silently losing text or substituting another kind. The app remains on the imported original after export; successful export is a separate PDF, not save-in-place. Unsaved annotations are still session-only if Android kills the process. This is therefore a **partial P0 export implementation**, not completion of all durable editing requirements.

## Build and checks

- `tools/build-apk.sh Debug`: succeeded for `arm64-v8a` and `x86_64`; Gradle `lintDebug` succeeded. That earlier APK was deleted during workspace cleanup; the latest previously built APK is `/home/user/folio-android-parity-debug.apk` (not rebuilt for this Windows-focused pass).
- PDFium writer round-trip test: seven kinds reopen with nonempty `/AP`, change rendered page pixels, preserve source bytes; free-text export fails without a staged file. A rotated-page test checks the mark lands in the displayed rectangle. Full Linux build and `ctest` passed **14/14** with Skia (the container needed `-DFOLIO_SKIA_GL_LIB=/usr/lib/x86_64-linux-gnu/libGL.so.1` because its development symlink is missing).
- JNI symbol check confirmed `nativeOnSaveResult` in the Android arm64 library. **No on-device provider, rotation, offline, low-space or process-death test has been performed here.** A Linux render test and a successful APK build are not Android acceptance.

## Manual Android acceptance still needed

1. Import a two-page PDF using Files; draw each supported kind, export with Save to a local document provider, open the exported PDF in Folio and another PDF viewer. Confirm colour, location and visibility, especially on `/Rotate` 90/270 and nonzero crop boxes.
2. Repeat to a cloud/offline provider and deliberately exhaust space or revoke provider permission; verify visible failure and intact in-session marks. Confirm no partial file (or display a cleanup warning if provider deletion fails).
3. Cancel the picker; confirm marks remain and retry succeeds. Export twice; inspect annotation count for duplicates. Switch documents during preparation and during the picker; confirm the original source remains untouched.
4. Kill the app before and after provider export, restart and check the destination bytes. Work not yet exported is not protected against process death.

## Windows Clang

This Linux sandbox has no Windows SDK. The user subsequently ran the Windows-hosted **ClangCL** GitHub Actions workflow: the application compiled, but `engine.pdfium` failed one 2× antialiasing exactness check (42 differing subpixels, worst 1/255), leaving 13/14 tests passing. That test is corrected in the source; the workflow now uploads a compiled artifact marked **UNVERIFIED** even on test failure. The deliverable template is at `github-workflows/windows-clang.yml`; copy it into `.github/workflows/windows-clang.yml` in your GitHub repository with the source changes and rerun. The previous failed run uploaded no artifact, so its binary is not recoverable from that runner. Only a new green run verifies all tests and packaged executable loading. See [the updated Windows report](WINDOWS-CLANG-PARITY-PASS.md).
