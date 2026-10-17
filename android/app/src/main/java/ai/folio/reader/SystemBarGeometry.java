package ai.folio.reader;

/** Screen-pixel geometry for NativeActivity's WINDOW-owned EGL surface. */
final class SystemBarGeometry {
    private SystemBarGeometry() {}

    /** The part of the Android status bar not already above this window/view. */
    static int uncovered(int barBottomOnScreen, int originOnScreen) {
        return Math.max(0, barBottomOnScreen - originOnScreen);
    }
}
