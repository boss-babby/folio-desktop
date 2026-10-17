# Thumb menu and scrollbar behaviour — 2026-10-17

## Menu geometry and state

- On **known blank paper**, the stationary hold now opens the tool arc at **0.50 seconds**; a press on actual text retains text-selection priority at 0.50 seconds. Dragging before the hold continues the normal page/annotation gesture.
- The first tier has four compact 24dp icons at ±66/±22dp, with **44dp hit targets**. Its borderless curved card ends 67dp above the held point. The icon-only property card starts 62dp above that point: **a visible 5dp gap**, rather than the previous 26dp overlap. It is 208×58dp, with four 44dp targets 46dp apart. Tool/property captions are removed.
- The value card sits above the property card with a 4dp gap. The full 24-colour grid needs the tool arc's vertical space, so the arc yields that upper slot **only while a value tier is visible**. The property row and value grid never overlap. All colours, six sizes, four opacities and five Pen shapes are retained.
- Releasing a **blank hold** leaves first-tier tools visible. Selecting Hand/Pan (which has no properties) and releasing closes with the reverse popup reveal. Finishing a **single uninterrupted** tool → property → value sweep also commits once and closes with that animation. A tool with more options selected on the first lift leaves its property row visible. When a **separate tap** enters the already pinned palette, picking a value leaves property and value tiers available to edit another property, without a new hold. No pinned tier schedules idle frames.
- An outside press, including a tap on the command bar or side panel, starts the **same 200 ms reverse popup reveal** and still performs the underlying page/chrome action. It does not abruptly clear the visual on the Down. A new hold may arm while that closing image fades. Starting a page zoom (toolbar/wheel/pinch/held horizontal corridor) dismisses it. The palette itself does not exit Focus. Small retained PDF damage and the text-selection/native card reveal from the previous performance pass are unchanged.

## Scrollbar behaviour

- The PDF viewport's **vertical and horizontal bars** and shared `ScrollView` bars (thumbnail/outline/panel lists) now start a drag only when the actual thumb is grabbed. A touch retains the wider cross-axis hit area but must be within the thumb's **length**; tapping or moving after a press on the empty rail never seeks/jumps or paints the page beneath it.
- Mouse track clicks are inert too. A genuine thumb drag, wheel/trackpad scrolling, finger page-pan, and the existing elastic panel edges still work.

## Verification and limits

Full Linux native suite: **19/19 passed**; focused mobile gestures/UI: **89 checks passed**, including Hand and uninterrupted third-tier exit, a pinned value change, rail orientations, zoom and animated outside dismissal. Navigation/recents tests: **78 checks passed**, including mobile percentage zoom presets and right-panel Undo/Redo visibility. Android `assembleDebug` and `lintDebug` succeeded; ZIP integrity and both arm64-v8a/x86_64 PDFium + Folio native libraries were checked. APK: `/home/user/folio-android-zoom-undo-debug.apk` (SHA-256 `ffe45719972627f681bc7b5e9942708434fa2815c04e3c9a525adf003a698080`). No physical phone or emulator is available here, so actual finger feel is not claimed as measured.

The expanded right inspector now carries pinned Undo and Redo icons next to Focus, using the existing per-document edit history and enabled only when applicable; desktop top-bar shortcuts are unchanged. The bottom zoom percentage is now a hit target for Folio's normal upward Popover: Fit width, Fit page and 50%, 75%, 100%, 125%, 150%, 200%. Compact phones keep the old separate fit buttons hidden; larger screens retain them. Storage modes are a design-only decision documented in `PDF-STORAGE-MODES-DESIGN.md`, not shipped in this build.
