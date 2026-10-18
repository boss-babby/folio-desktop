# Windows chrome and access-violation triage

## What was reproduced from source/screenshot

- The 1440 × 900 default window was created at that size even on a smaller laptop; its right and bottom edges (including the Close button) could be outside the visible monitor.
- The frameless Win32 shell already supported `beginDragMove()`, but no UI handler ever called it. The caption therefore could not be dragged when restored.
- The maximised client inset was computed using system-DPI frame metrics instead of the current monitor's work area; on mixed-DPI machines this can clip the title bar.
- `wglGetProcAddress()` results were treated as callable whenever non-null, although Windows can return the documented invalid sentinel values `1`, `2`, `3`, or `-1`. Now every WGL lookup validates these before Skia receives a pointer.
- The WGL DC was released by the member destructor **after** its owning HWND was destroyed; it is now shut down while the window is valid.
- Desktop cards incorrectly said `Offline copy` when they actually opened the original filesystem PDF. They now say `Original file`; the Android Keep copy wording is unchanged.

## Changes

- `folio-platform/win32/Win32Window.cpp`: initial maximised window fits the monitor *work area*, taskbar remains accessible; restored size is centred/clamped to the work area. Fixed maximise insets, WGL function pointer validation and DC shutdown order.
- `folio-ui/screens/FolioApp.cpp`: native OS move on an empty integrated title-bar mouse press, maximise/restore on double-click; controls/tabs/popovers are not stolen. Initial maximise button state now reflects the actual native window.
- `folio-ui/components/{Button,WindowChrome}.cpp` and `folio-ui/include/folio/ui/Tokens.h`: smaller Windows caption glyphs, still usable hit targets.
- `folio-ui/shell/main_native.cpp`: on Windows, a small **local** unhandled-crash minidump is attempted under `%LOCALAPPDATA%\Folio\crashes\`. Windows Error Reporting continues normally. A dump may include PDF text; never upload it publicly or without the user's consent.
- `folio-ui/shell/NativeShell.cpp`: warn if the executable was moved without its packaged `assets/fonts` folder.
- Native navigation test covers drag and double-click without stealing the Close button.

## Verification boundary and next check

The headless Linux UI build passed 8/8 CTests; the navigation test passed 79 checks. This cannot certify Win32 layout, GPU drivers, or PDF crashes. The supplied Event Viewer entry has `0xc0000005`, module `unknown`, offset `-8`; **that alone does not identify the crashing function**. The WGL changes remove a real invalid-function-pointer hazard, but a new Windows run is needed to establish whether they eliminate this particular crash.

1. Commit/push the changed source and workflow prerequisites, rerun the Windows full-linked GitHub Action, and use **its new ZIP**. Extract the whole ZIP into a folder; do not move Folio.exe alone to Pictures.
2. Open multiple PDFs and leave one open through the previous crash interval. Restore and drag the window by its empty title bar; verify taskbar, bottom row and right controls stay visible on your monitor and after a DPI change.
3. If there is still a crash, run `Folio.exe --software` in the extracted folder and compare. Report whether it also crashes, the Event Viewer exception code, and (if you choose to share privately) the file from `%LOCALAPPDATA%\Folio\crashes\`. Keep PDFs, sidecars and dumps private; no deletion of recents or annotations is required.
